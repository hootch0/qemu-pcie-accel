// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe Accelerator Device - P2P DMA Support
 *
 * Copyright (C) 2026
 *
 * This file implements peer-to-peer DMA functionality:
 * - P2P peer device discovery and registration
 * - Peer BAR mapping
 * - P2P DMA setup via admin commands
 * - IOMMU/PASID integration (where available)
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#ifdef CONFIG_PCI_P2PDMA
#include <linux/pci-p2pdma.h>
#endif

#include "pcie-accel.h"

/*
 * ===== P2P Peer Management =====
 */

/**
 * accel_find_p2p_peer - Find a registered P2P peer by BDF
 * @dev: Device structure
 * @bdf: Bus:Device:Function identifier
 *
 * Returns: Peer structure if found, NULL otherwise
 */
static struct accel_p2p_peer *accel_find_p2p_peer(struct accel_dev *dev,
						   u16 bdf)
{
	struct accel_p2p_peer *peer;
	unsigned long flags;

	spin_lock_irqsave(&dev->p2p_lock, flags);
	list_for_each_entry(peer, &dev->p2p_peers, list) {
		if (peer->bdf == bdf) {
			spin_unlock_irqrestore(&dev->p2p_lock, flags);
			return peer;
		}
	}
	spin_unlock_irqrestore(&dev->p2p_lock, flags);

	return NULL;
}

/**
 * bdf_to_pci_device - Convert BDF to PCI device
 * @dev: Our device (for bus reference)
 * @bdf: Bus:Device:Function identifier
 *
 * Finds a PCI device by its BDF. The BDF format is:
 * Bits [15:8] = Bus number
 * Bits [7:3]  = Device number
 * Bits [2:0]  = Function number
 *
 * Returns: PCI device if found (reference taken), NULL otherwise
 */
static struct pci_dev *bdf_to_pci_device(struct accel_dev *dev, u16 bdf)
{
	struct pci_bus *bus;
	struct pci_dev *pdev;
	unsigned int bus_num = (bdf >> 8) & 0xFF;
	unsigned int devfn = bdf & 0xFF;

	/* Find the bus */
	bus = pci_find_bus(pci_domain_nr(dev->pdev->bus), bus_num);
	if (!bus) {
		dev_warn(&dev->pdev->dev, "P2P: Bus %u not found\n", bus_num);
		return NULL;
	}

	/* Find the device on the bus */
	pdev = pci_get_slot(bus, devfn);
	if (!pdev) {
		dev_warn(&dev->pdev->dev, "P2P: Device %02x:%02x.%x not found\n",
			 bus_num, PCI_SLOT(devfn), PCI_FUNC(devfn));
		return NULL;
	}

	return pdev;
}

/**
 * accel_setup_p2p_peer - Set up a P2P peer device
 * @dev: Device structure
 * @peer_bdf: Peer device BDF
 *
 * Registers a peer device for P2P DMA transfers. This involves:
 * 1. Finding the peer PCI device
 * 2. Mapping the peer's BAR for direct access
 * 3. Sending P2P setup command to device
 * 4. Adding peer to our tracking list
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_setup_p2p_peer(struct accel_dev *dev, u16 peer_bdf)
{
	struct accel_p2p_peer *peer;
	struct pci_dev *peer_pdev;
	struct accel_cmd cmd;
	struct accel_cqe cqe;
	unsigned long flags;
	int ret;

	/* Check if already registered - idempotent, return success */
	if (accel_find_p2p_peer(dev, peer_bdf)) {
		dev_dbg(&dev->pdev->dev, "P2P peer 0x%04x already registered\n",
			peer_bdf);
		return 0;
	}

	/* Check peer count limit */
	spin_lock_irqsave(&dev->p2p_lock, flags);
	{
		int count = 0;
		struct accel_p2p_peer *p;
		list_for_each_entry(p, &dev->p2p_peers, list)
			count++;
		if (count >= ACCEL_MAX_P2P_PEERS) {
			spin_unlock_irqrestore(&dev->p2p_lock, flags);
			dev_err(&dev->pdev->dev, "P2P peer limit reached\n");
			return -ENOSPC;
		}
	}
	spin_unlock_irqrestore(&dev->p2p_lock, flags);

	/* Find the peer PCI device */
	peer_pdev = bdf_to_pci_device(dev, peer_bdf);
	if (!peer_pdev)
		return -ENODEV;

	/* Allocate peer structure */
	peer = kzalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer) {
		pci_dev_put(peer_pdev);
		return -ENOMEM;
	}

	peer->bdf = peer_bdf;
	peer->pdev = peer_pdev;

	/*
	 * Check if P2P DMA is possible between these devices.
	 * This requires them to be on the same PCIe switch or root complex.
	 * Note: QEMU virtual devices don't expose proper topology info,
	 * so this check will fail - but QEMU handles P2P internally.
	 */
#ifdef CONFIG_PCI_P2PDMA
	{
		int distance = pci_p2pdma_distance(dev->pdev, &peer_pdev->dev, false);
		if (distance < 0) {
			dev_dbg(&dev->pdev->dev,
				"P2P DMA topology check failed for peer 0x%04x (distance=%d), "
				"continuing (QEMU handles P2P internally)\n",
				peer_bdf, distance);
		}
	}
#endif

	/*
	 * Map the peer's BAR4 (CMB) for P2P transfers.
	 * This is where P2P data is stored. BAR4 is a RAM-backed region
	 * that can be directly accessed for peer-to-peer DMA operations.
	 */
	if (pci_resource_len(peer_pdev, 4) > 0) {
		peer->mem = pci_iomap(peer_pdev, 4, 0);
		if (peer->mem) {
			peer->mem_size = pci_resource_len(peer_pdev, 4);
			peer->mem_phys = pci_resource_start(peer_pdev, 4);
			dev_info(&dev->pdev->dev,
				"P2P: Mapped peer BAR4 CMB: %pR (phys=0x%llx)\n",
				&peer_pdev->resource[4],
				(unsigned long long)peer->mem_phys);
		} else {
			dev_warn(&dev->pdev->dev,
				"P2P: Failed to map peer BAR4\n");
		}
	} else {
		dev_warn(&dev->pdev->dev,
			"P2P: Peer has no BAR4 CMB memory\n");
	}

	/*
	 * Send P2P setup admin command to device.
	 * This tells the QEMU device about the peer for P2P operations.
	 *
	 * CDW10[15:0] = peer BDF
	 * CDW10[16]   = operation (0=register, 1=unregister)
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = ACCEL_ADM_CMD_P2P_SETUP;
	cmd.dw.admin.cdw10 = cpu_to_le32(peer_bdf);  /* Register (bit 16 = 0) */

	ret = accel_submit_admin_cmd(dev, &cmd, &cqe);
	if (ret) {
		dev_err(&dev->pdev->dev,
			"P2P setup admin command failed: %d\n", ret);
		goto err_unmap;
	}

	/* Check command status */
	if ((le16_to_cpu(cqe.status) >> 1) != 0) {
		dev_err(&dev->pdev->dev,
			"P2P setup failed: device status 0x%x\n",
			le16_to_cpu(cqe.status) >> 1);
		ret = -EIO;
		goto err_unmap;
	}

	/* Add to peer list */
	spin_lock_irqsave(&dev->p2p_lock, flags);
	list_add_tail(&peer->list, &dev->p2p_peers);
	spin_unlock_irqrestore(&dev->p2p_lock, flags);

	dev_info(&dev->pdev->dev,
		 "P2P peer registered: %04x:%02x:%02x.%x\n",
		 pci_domain_nr(peer_pdev->bus),
		 peer_pdev->bus->number,
		 PCI_SLOT(peer_pdev->devfn),
		 PCI_FUNC(peer_pdev->devfn));

	return 0;

err_unmap:
	if (peer->mem)
		pci_iounmap(peer_pdev, peer->mem);
	pci_dev_put(peer_pdev);
	kfree(peer);
	return ret;
}

/**
 * accel_remove_p2p_peer - Remove a P2P peer
 * @dev: Device structure
 * @peer_bdf: Peer device BDF
 *
 * Unregisters a peer device.
 *
 * Returns: 0 on success, negative error code on failure
 */
static int __maybe_unused accel_remove_p2p_peer(struct accel_dev *dev, u16 peer_bdf)
{
	struct accel_p2p_peer *peer;
	struct accel_cmd cmd;
	unsigned long flags;

	/* Find the peer */
	peer = accel_find_p2p_peer(dev, peer_bdf);
	if (!peer)
		return -ENOENT;

	/*
	 * Send P2P teardown admin command.
	 * CDW10[15:0] = peer BDF
	 * CDW10[16]   = operation (1=unregister)
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = ACCEL_ADM_CMD_P2P_SETUP;
	cmd.dw.admin.cdw10 = cpu_to_le32(peer_bdf | (1 << 16));

	/* Best effort - don't fail if command fails */
	accel_submit_admin_cmd(dev, &cmd, NULL);

	/* Remove from list */
	spin_lock_irqsave(&dev->p2p_lock, flags);
	list_del(&peer->list);
	spin_unlock_irqrestore(&dev->p2p_lock, flags);

	/* Cleanup */
	if (peer->mem)
		pci_iounmap(peer->pdev, peer->mem);
	pci_dev_put(peer->pdev);
	kfree(peer);

	dev_info(&dev->pdev->dev, "P2P peer 0x%04x unregistered\n", peer_bdf);

	return 0;
}

/**
 * accel_cleanup_p2p_peers - Remove all P2P peers
 * @dev: Device structure
 *
 * Called during device removal to clean up all registered peers.
 * Sends teardown commands to device before freeing local resources.
 */
void accel_cleanup_p2p_peers(struct accel_dev *dev)
{
	struct accel_p2p_peer *peer, *tmp;
	struct accel_cmd cmd;
	unsigned long flags;
	LIST_HEAD(remove_list);

	/* Move all peers to a temporary list */
	spin_lock_irqsave(&dev->p2p_lock, flags);
	list_splice_init(&dev->p2p_peers, &remove_list);
	spin_unlock_irqrestore(&dev->p2p_lock, flags);

	/* Remove each peer */
	list_for_each_entry_safe(peer, tmp, &remove_list, list) {
		/*
		 * Send P2P teardown admin command.
		 * CDW10[15:0] = peer BDF
		 * CDW10[16]   = operation (1=unregister)
		 */
		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = ACCEL_ADM_CMD_P2P_SETUP;
		cmd.dw.admin.cdw10 = cpu_to_le32(peer->bdf | (1 << 16));

		/* Best effort - don't fail removal if command fails */
		accel_submit_admin_cmd(dev, &cmd, NULL);

		list_del(&peer->list);

		if (peer->mem)
			pci_iounmap(peer->pdev, peer->mem);
		pci_dev_put(peer->pdev);
		kfree(peer);

		dev_dbg(&dev->pdev->dev, "P2P peer 0x%04x unregistered\n",
			peer->bdf);
	}
}

/*
 * ===== PASID/SVA Support =====
 * Note: Full PASID support requires kernel IOMMU SVA infrastructure
 * which may not be available on all systems.
 */

#ifdef CONFIG_IOMMU_SVA
/**
 * accel_enable_pasid - Enable PASID support for the device
 * @dev: Device structure
 *
 * Attempts to enable PASID for shared virtual addressing using the
 * modern IOMMU SVA API. The legacy pci_*pasid functions are deprecated.
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_enable_pasid(struct accel_dev *dev)
{
	struct iommu_sva *sva;

	/* Bind to current process's page tables via SVA */
	sva = iommu_sva_bind_device(&dev->pdev->dev, current->mm);
	if (IS_ERR(sva)) {
		int ret = PTR_ERR(sva);
		dev_warn(&dev->pdev->dev, "Failed to bind SVA: %d\n", ret);
		return ret;
	}

	dev->pasid = iommu_sva_get_pasid(sva);
	dev->pasid_enabled = true;

	dev_info(&dev->pdev->dev, "PASID enabled: %u\n", dev->pasid);

	return 0;
}

/**
 * accel_disable_pasid - Disable PASID support
 * @dev: Device structure
 */
void accel_disable_pasid(struct accel_dev *dev)
{
	if (!dev->pasid_enabled)
		return;

	/* SVA unbind is handled automatically when the mm is destroyed */
	dev->pasid_enabled = false;
	dev->pasid = 0;
}
#else
int accel_enable_pasid(struct accel_dev *dev)
{
	return -ENODEV;
}

void accel_disable_pasid(struct accel_dev *dev)
{
}
#endif /* CONFIG_IOMMU_SVA */

/*
 * ===== P2P DMA Helpers =====
 */

/**
 * accel_p2p_dma_map - Map memory for P2P DMA
 * @dev: Source device
 * @peer: Target peer device
 * @addr: Virtual address to map
 * @len: Length to map
 * @dma_addr: Output DMA address
 *
 * Maps a buffer for P2P DMA transfer. This may use the kernel's
 * P2PDMA infrastructure if available, or fall back to regular DMA.
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_p2p_dma_map(struct accel_dev *dev, struct accel_p2p_peer *peer,
		      void *addr, size_t len, dma_addr_t *dma_addr)
{
	/*
	 * For now, we rely on the device (QEMU) to handle the P2P routing.
	 * The driver just passes through the host virtual address which
	 * gets translated by the device's DMA operations.
	 *
	 * In a production driver with real hardware, we would use:
	 * - pci_p2pdma_map_sg() for P2P between devices
	 * - dma_map_single() for regular host memory
	 */
	*dma_addr = dma_map_single(&dev->pdev->dev, addr, len, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(&dev->pdev->dev, *dma_addr))
		return -EIO;

	return 0;
}

/**
 * accel_p2p_dma_unmap - Unmap P2P DMA buffer
 * @dev: Device structure
 * @dma_addr: DMA address to unmap
 * @len: Length
 */
void accel_p2p_dma_unmap(struct accel_dev *dev, dma_addr_t dma_addr, size_t len)
{
	dma_unmap_single(&dev->pdev->dev, dma_addr, len, DMA_BIDIRECTIONAL);
}

/*
 * ===== P2P MMIO Queue Support =====
 */

/**
 * accel_p2p_queue_setup - Set up a P2P MMIO queue pair with a peer
 * @dev: Device structure
 * @params: P2P queue setup parameters
 *
 * Issues an admin command to the device to set up a P2P MMIO queue pair.
 * The peer's BAR addresses are read from PCI config space if not provided.
 *
 * Returns: 0 on success, negative errno on failure
 */
int accel_p2p_queue_setup(struct accel_dev *dev,
			  struct accel_p2p_queue_setup *params)
{
	struct accel_cmd cmd = {};
	struct accel_cqe cqe = {};
	struct pci_dev *peer_pdev;
	resource_size_t peer_bar0, peer_bar4;
	int ret;

	if (params->slot >= ACCEL_P2Q_MAX_SLOTS ||
	    params->peer_slot >= ACCEL_P2Q_MAX_SLOTS)
		return -EINVAL;

	/* If BAR addresses not provided, read from peer's PCI config */
	if (params->peer_bar0 == 0 || params->peer_bar4 == 0) {
		peer_pdev = pci_get_domain_bus_and_slot(
			pci_domain_nr(dev->pdev->bus),
			(params->peer_bdf >> 8) & 0xFF,
			PCI_DEVFN((params->peer_bdf >> 3) & 0x1F,
				  params->peer_bdf & 0x7));
		if (!peer_pdev) {
			dev_err(&dev->pdev->dev,
				"P2P queue: peer 0x%x not found\n",
				params->peer_bdf);
			return -ENODEV;
		}

		peer_bar0 = pci_resource_start(peer_pdev, 0);
		peer_bar4 = pci_resource_start(peer_pdev, 4);
		pci_dev_put(peer_pdev);

		if (!peer_bar0 || !peer_bar4) {
			dev_err(&dev->pdev->dev,
				"P2P queue: peer 0x%x BAR not mapped\n",
				params->peer_bdf);
			return -EINVAL;
		}
	} else {
		peer_bar0 = params->peer_bar0;
		peer_bar4 = params->peer_bar4;
	}

	/* Build P2P queue setup admin command */
	cmd.opcode = ACCEL_ADM_CMD_P2P_QUEUE_SETUP;
	cmd.dw.admin.cdw10 = cpu_to_le32(
		(params->peer_bdf & 0xFFFF) |
		((params->slot & 0xF) << 16) |
		((params->peer_slot & 0xF) << 20));
	cmd.dw.admin.cdw11 = cpu_to_le32(peer_bar0 & 0xFFFFFFFF);
	cmd.dw.admin.cdw12 = cpu_to_le32((peer_bar0 >> 32) & 0xFFFFFFFF);
	cmd.dw.admin.cdw13 = cpu_to_le32(peer_bar4 & 0xFFFFFFFF);
	cmd.dw.admin.cdw14 = cpu_to_le32((peer_bar4 >> 32) & 0xFFFFFFFF);

	ret = accel_submit_admin_cmd(dev, &cmd, &cqe);
	if (ret) {
		dev_err(&dev->pdev->dev,
			"P2P queue setup failed: slot=%u peer=0x%x ret=%d\n",
			params->slot, params->peer_bdf, ret);
		return ret;
	}

	dev_info(&dev->pdev->dev,
		 "P2P queue setup: slot=%u peer=0x%x peer_slot=%u "
		 "bar0=0x%llx bar4=0x%llx\n",
		 params->slot, params->peer_bdf, params->peer_slot,
		 (unsigned long long)peer_bar0, (unsigned long long)peer_bar4);

	return 0;
}
