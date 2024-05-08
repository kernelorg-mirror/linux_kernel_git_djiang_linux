// SPDX-License-Identifier: GPL-2.0
/*
 * MMIO Mailbox
 *
 * Copyright (C) 2024 Intel Corporation
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/cxl.h>
#include "pci.h"

/*
 * PCIe Base spec r6.2 6.35.1.3.2.1 "MMB Capability Register" defines a
 * field to dictate how long to wait for the mailbox to become ready. The
 * field allows the device to tell software the amount of time to wait
 * before mailbox ready. This field per the spec theoretically allows
 * for up to 255 seconds. 255 seconds is unreasonably long, its longer
 * than the maximum SATA port link recovery wait. Default to 60 seconds
 * until someone builds a CXL device that needs more time in practice.
 */
static unsigned short mbox_ready_timeout = 60;
module_param(mbox_ready_timeout, ushort, 0644);
MODULE_PARM_DESC(mbox_ready_timeout, "seconds to wait for mailbox ready");

/* MMB Registers. PCIe base spec r6.2 6.35.1.3.2 */
#define PCI_MMB_CAPS_OFFSET 0x00
#define   PCI_MMB_CAP_PAYLOAD_SIZE_MASK GENMASK(4, 0)
#define   CXL_MMB_CAP_BG_CMD_IRQ BIT(6)
#define   PCI_MMB_CAP_IRQ_MSGNUM_MASK GENMASK(10, 7)
#define PCI_MMB_CTRL_OFFSET 0x04
#define   PCI_MMB_CTRL_DOORBELL BIT(0)
#define   CXL_MMB_CTRL_BG_CMD_IRQ BIT(2)
#define PCI_MMB_CMD_OFFSET 0x08
#define   PCI_MMB_CMD_COMMAND_OPCODE_MASK GENMASK_ULL(15, 0)
#define   PCI_MMB_CMD_PAYLOAD_LENGTH_MASK GENMASK_ULL(36, 16)
#define PCI_MMB_STATUS_OFFSET 0x10
#define   CXL_MMB_STATUS_BG_CMD		BIT(0)
#define   PCI_MMB_STATUS_READY		BIT(1)
#define   PCI_MMB_STATUS_ATTN		BIT(2)
#define   PCI_MMB_STATUS_RET_CODE_MASK	GENMASK_ULL(47, 32)
#define CXL_MBOX_BG_STATUS_OFFSET	0x18
#define PCI_MMB_PAYLOAD_OFFSET		0X20

#define mmb_doorbell_busy(mbox)                                                \
	(readl((mbox)->mbox_ctrl_addr + PCI_MMB_CTRL_OFFSET) &                 \
	 PCI_MMB_CTRL_DOORBELL)

/* PCIe Base spec r6.2 6.35.1.3 */
#define PCI_MMB_TIMEOUT_MS (2 * HZ)

static bool pci_mmb_ready(struct mmio_mailbox *mbox)
{
	u64 status;

	status = readq(mbox->mbox_ready_addr + PCI_MMB_STATUS_OFFSET);

	return !!(status & PCI_MMB_STATUS_READY);
}

static int pci_mmb_wait_for_doorbell(struct mmio_mailbox *mbox)
{
	const unsigned long start = jiffies;
	struct device *dev = mbox->dev;
	unsigned long end = start;

	while (mmb_doorbell_busy(mbox)) {
		end = jiffies;

		if (time_after(end, start + PCI_MMB_TIMEOUT_MS)) {
			/* Check again in case preempted before timeout test */
			if (!mmb_doorbell_busy(mbox))
				break;
			return -ETIMEDOUT;
		}
		cpu_relax();
	}

	dev_dbg(dev, "Doorbell wait took %dms",
		jiffies_to_msecs(end) - jiffies_to_msecs(start));
	return 0;
}

static int mmb_send_prep(struct mmio_mailbox *mbox, struct mmio_mbox_cmd *cmd)
{
	if (!mbox->ops || !mbox->ops->cmd_prep)
		return -EOPNOTSUPP;

	return mbox->ops->cmd_prep(mbox, cmd);
}

static int mmb_send_done(struct mmio_mailbox *mbox, struct mmio_mbox_cmd *cmd)
{
	if (!mbox->ops || !mbox->ops->cmd_done)
		return -EOPNOTSUPP;

	return mbox->ops->cmd_done(mbox, cmd);
}

static int mmb_ready(struct mmio_mailbox *mbox)
{
	if (!mbox->ops || !mbox->ops->mbox_ready)
		return -EOPNOTSUPP;

	return mbox->ops->mbox_ready(mbox);
}

/**
 * __cxl_pci_mbox_send_cmd() - Execute a mailbox command
 * @mds: The memory device driver data
 * @mbox_cmd: Command to send to the memory device.
 *
 * Context: Any context. Expects mbox_mutex to be held.
 * Return: -ETIMEDOUT if timeout occurred waiting for completion. 0 on success.
 *         Caller should check the return code in @mbox_cmd to make sure it
 *         succeeded.
 *
 * This is a generic form of the CXL mailbox send command thus only using the
 * registers defined by the mailbox capability ID - CXL 2.0 8.2.8.4. Memory
 * devices, and perhaps other types of CXL devices may have further information
 * available upon error conditions. Driver facilities wishing to send mailbox
 * commands should use the wrapper command.
 *
 * The CXL spec allows for up to two mailboxes. The intention is for the primary
 * mailbox to be OS controlled and the secondary mailbox to be used by system
 * firmware. This allows the OS and firmware to communicate with the device and
 * not need to coordinate with each other. The driver only uses the primary
 * mailbox.
 */
static int __mmio_mailbox_send_cmd(struct mmio_mailbox *mbox,
				   struct mmio_mbox_cmd *mbox_cmd)
{
	void __iomem *payload = mbox->mbox_ctrl_addr + PCI_MMB_PAYLOAD_OFFSET;
	struct device *dev = mbox->dev;
	u64 cmd_reg, status_reg;
	size_t out_len;
	int rc;

	lockdep_assert_held(&mbox->mbox_mutex);

	/*
	 * Here are the steps from '6.35.1.3.1 MMB Operation' of PCIe Base Spec r6.2
	 *   1. Caller reads MB Control Register to verify doorbell is clear
	 *   2. Caller writes Command Register
	 *   3. Caller writes Command Payload Registers if input payload is non-empty
	 *   4. Caller writes MB Control Register to set doorbell
	 *   5. Caller either polls for doorbell to be clear or waits for interrupt if configured
	 *   6. Caller reads MB Status Register to fetch Return code
	 *   7. If command successful, Caller reads Command Register to get Payload Length
	 *   8. If output payload is non-empty, host reads Command Payload Registers
	 *
	 * Hardware is free to do whatever it wants before the doorbell is rung,
	 * and isn't allowed to change anything after it clears the doorbell. As
	 * such, steps 2 and 3 can happen in any order, and steps 6, 7, 8 can
	 * also happen in any order (though some orders might not make sense).
	 */

	/* #1 */
	if (mmb_doorbell_busy(mbox)) {
		dev_warn(dev, "mailbox queue busy");
		return -EBUSY;
	}

	rc = mmb_send_prep(mbox, mbox_cmd);
	if (rc < 0)
		return rc;

	cmd_reg = FIELD_PREP(PCI_MMB_CMD_COMMAND_OPCODE_MASK,
			     mbox_cmd->opcode);
	if (mbox_cmd->size_in) {
		if (WARN_ON(!mbox_cmd->payload_in))
			return -EINVAL;

		cmd_reg |= FIELD_PREP(PCI_MMB_CMD_PAYLOAD_LENGTH_MASK,
				      mbox_cmd->size_in);
		memcpy_toio(payload, mbox_cmd->payload_in, mbox_cmd->size_in);
	}

	/* #2, #3 */
	writeq(cmd_reg, mbox->mbox_ctrl_addr + PCI_MMB_CMD_OFFSET);

	/* #4 */
	dev_dbg(dev, "Sending command: 0x%04x\n", mbox_cmd->opcode);
	writel(PCI_MMB_CTRL_DOORBELL, mbox->mbox_ctrl_addr + PCI_MMB_CTRL_OFFSET);

	/* #5 */
	rc = pci_mmb_wait_for_doorbell(mbox);
	if (rc == -ETIMEDOUT) {
		dev_warn(dev, "mailbox timeout");
		return rc;
	}

	/* #6 */
	status_reg = readq(mbox->mbox_ctrl_addr + PCI_MMB_STATUS_OFFSET);
	mbox_cmd->return_code = FIELD_GET(PCI_MMB_STATUS_RET_CODE_MASK,
					  status_reg);

	rc = mmb_send_done(mbox, mbox_cmd);
	if (rc < 0)
		return rc;
	if (rc == 1)
		goto success;

	if (mbox_cmd->return_code != 0)
		return 0; /* completed but caller must check return_code */

success:
	/* #7 */
	cmd_reg = readq(mbox->mbox_ctrl_addr + PCI_MMB_CMD_OFFSET);
	out_len = FIELD_GET(PCI_MMB_CMD_PAYLOAD_LENGTH_MASK, cmd_reg);

	/* #8 */
	if (out_len && mbox_cmd->payload_out) {
		/*
		 * Sanitize the copy. If hardware misbehaves, out_len per the
		 * spec can actually be greater than the max allowed size (21
		 * bits available but spec defined 1M max). The caller also may
		 * have requested less data than the hardware supplied even
		 * within spec.
		 */
		size_t n;

		n = min3(mbox_cmd->size_out, mbox->payload_size, out_len);
		memcpy_fromio(mbox_cmd->payload_out, payload, n);
		mbox_cmd->size_out = n;
	} else {
		mbox_cmd->size_out = 0;
	}

	return 0;
}

int mmio_mailbox_send(struct mmio_mailbox *mbox, struct mmio_mbox_cmd *cmd)
{
	int rc;

	if (!mbox->ops || !mbox->ops->mbox_send)
		return -EOPNOTSUPP;

	mutex_lock_io(&mbox->mbox_mutex);
	rc = __mmio_mailbox_send_cmd(mbox, cmd);
	mutex_unlock(&mbox->mbox_mutex);

	return rc;
}
EXPORT_SYMBOL_GPL(mmio_mailbox_send);

static const struct mmio_mbox_ops pci_mbox_ops = {
	.mbox_ready = pci_mmb_ready,
	.mbox_send = mmio_mailbox_send,
};

int mmio_setup_mailbox(struct mmio_mailbox *mbox)
{
	const int cap = readl(mbox->mbox_ctrl_addr + PCI_MMB_CAPS_OFFSET);
	struct device *dev = mbox->dev;
	unsigned long timeout;
	bool mbox_ready;

	mutex_init(&mbox->mbox_mutex);

	timeout = jiffies + mbox_ready_timeout * HZ;
	do {
		mbox_ready = mmb_ready(mbox);
		if (mbox_ready)
			break;
		if (msleep_interruptible(100))
			break;
	} while (!time_after(jiffies, timeout));

	if (!mbox_ready) {
		dev_err(dev, "timedout awaiting mailbox ready");
		return -ETIMEDOUT;
	}

	/*
	 * A command may be in flight from a previous driver instance,
	 * think kexec, do one doorbell wait so that
	 * __cxl_pci_mbox_send_cmd() can assume that it is the only
	 * source for future doorbell busy events.
	 */
	if (pci_mmb_wait_for_doorbell(mbox) != 0) {
		dev_err(dev, "timeout awaiting mailbox idle");
		return -ETIMEDOUT;
	}

	mbox->payload_size = 1 << FIELD_GET(PCI_MMB_CAP_PAYLOAD_SIZE_MASK, cap);

	/*
	 * PCIe Base spec r6.2 6.35.1.3.2.1 "MMB Capability Register"
	 *
	 * If the size is too small, mandatory commands will not work and so
	 * there's no point in going forward. If the size is too large, there's
	 * no harm in soft limiting it.
	 */
	mbox->payload_size = min_t(size_t, mbox->payload_size, SZ_1M);
	if (mbox->payload_size < 256) {
		dev_err(dev, "Mailbox is too small (%zub)",
			mbox->payload_size);
		return -ENXIO;
	}

	dev_dbg(dev, "Mailbox payload sized %zu", mbox->payload_size);

	rcuwait_init(&mbox->mbox_wait);

	return 0;
}
EXPORT_SYMBOL_GPL(mmio_setup_mailbox);

int pci_mmio_setup_mailbox(struct pci_dev *pdev,
			   struct mmio_mailbox *mbox,
			   void __iomem *mmb_addr)
{
	mbox->dev = &pdev->dev;
	mbox->mbox_ready_addr = mmb_addr;
	mbox->mbox_ctrl_addr = mmb_addr;

	return mmio_setup_mailbox(mbox);
}
