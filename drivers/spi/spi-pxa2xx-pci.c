/*
 * CE4100's SPI device is more or less the same one as found on PXA
 *
 */
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/spi/pxa2xx_spi.h>
#include <linux/irq.h>
#include <linux/platform_data/clanton.h>

/* defined here to avoid including arch/x86/pci/intel_media_proc_gen3.c */
#define CE3100_SOC_DEVICE_ID 0x2E50
#define CE4100_SOC_DEVICE_ID 0x0708
#define CE4200_SOC_DEVICE_ID 0x0709
#define CE5300_SOC_DEVICE_ID 0x0C40
#define CE2600_SOC_DEVICE_ID 0x0931

#ifdef CONFIG_INTEL_QUARK_X1000_SOC_FPGAEMU
#define CE4200_NUM_SPI_MASTER 1
#else
#define CE4200_NUM_SPI_MASTER 2
#endif

#define CE4X00_SPI_MAX_SPEED  1843200

#ifdef CONFIG_INTEL_QUARK_X1000_SOC
#define CE4200_NUM_CHIPSELECT 2
#ifdef CONFIG_INTEL_QUARK_X1000_SOC_FPGAEMU
#define CE5X00_SPI_MAX_SPEED  3500000
#else
#define CE5X00_SPI_MAX_SPEED  50000000
#endif
#else
#define CE4200_NUM_CHIPSELECT 4
#define CE5X00_SPI_MAX_SPEED  5000000
#endif

#define SPI_CE_DEBUG

static int interface;

#ifdef CONFIG_INTEL_QUARK_X1000_SOC
static int enable_msi = 1;
#else
static int enable_msi;
#endif
module_param(enable_msi, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(enable_msi, "Enable PCI MSI mode");

static int ce4100_spi_probe(struct pci_dev *dev,
		const struct pci_device_id *ent)
{
	struct platform_device_info pi;
	int ret;
	struct platform_device *pdev;
	struct pxa2xx_spi_master spi_pdata;
	struct ssp_device *ssp;
	unsigned int id;

	ret = pcim_enable_device(dev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(dev, 1 << 0, "PXA2xx SPI");
	if (ret)
		return ret;

	memset(&spi_pdata, 0, sizeof(spi_pdata));
	spi_pdata.num_chipselect = CE4200_NUM_CHIPSELECT;

	ssp = &spi_pdata.ssp;
	ssp->pcidev = dev;
	ssp->phys_base = pci_resource_start(dev, 0);
	ssp->mmio_base = pcim_iomap_table(dev)[0];
	if (!ssp->mmio_base) {
		dev_err(&dev->dev, "failed to ioremap() registers\n");
		return -EIO;
	}
	ssp->irq = dev->irq;
	ssp->port_id = dev->devfn;
#ifdef CONFIG_INTEL_QUARK_X1000_SOC
	id = CE5300_SOC_DEVICE_ID;
#else
	intelce_get_soc_info(&id, NULL);
#endif
	switch (id) {
	case CE5300_SOC_DEVICE_ID:
		ssp->type = CE5X00_SSP;
		break;
	case CE4200_SOC_DEVICE_ID:
	default:
		ssp->type = CE4100_SSP;
		break;
	}

	memset(&pi, 0, sizeof(pi));
	pi.parent = &dev->dev;
	pi.name = "pxa2xx-spi";
	pi.id = ssp->port_id;
	pi.data = &spi_pdata;
	pi.size_data = sizeof(spi_pdata);

	pdev = platform_device_register_full(&pi);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	pdev->id = interface;
	pdev->dev.parent = &dev->dev;
#ifdef CONFIG_OF
	pdev->dev.of_node = dev->dev.of_node;
#endif
	pci_set_master(dev);
	if (enable_msi == 1) {
		ret = pci_enable_msi(dev);
		if (ret) {
			dev_err(&dev->dev, "failed to allocate MSI entry\n");
			platform_device_unregister(pdev);
			return ret;
		}
	}
	pci_set_drvdata(dev, pdev);

	interface++;

	return 0;
}

static void ce4100_spi_remove(struct pci_dev *dev)
{
	struct platform_device *pdev = pci_get_drvdata(dev);

	if (enable_msi == 1) {
		if (pci_dev_msi_enabled(dev))
			pci_disable_msi(dev);
	}

	platform_device_unregister(pdev);
}

#ifdef CONFIG_PM
static int ce4XXX_spi_suspend(struct pci_dev *dev, pm_message_t state)
{
	pci_save_state(dev);
	pci_set_power_state(dev, pci_choose_state(dev, state));
	return 0;
}

static int ce4XXX_spi_resume(struct pci_dev *dev)
{
	pci_set_power_state(dev, PCI_D0);
	pci_restore_state(dev);

	return 0;
}
#endif

static const struct pci_device_id ce4100_spi_devices[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x2e6a) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x0935) },
	{ },
};
MODULE_DEVICE_TABLE(pci, ce4100_spi_devices);

static struct pci_driver ce4100_spi_driver = {
	.name           = "ce4100_spi",
	.id_table       = ce4100_spi_devices,
	.probe          = ce4100_spi_probe,
#ifdef CONFIG_PM
	.suspend        = ce4XXX_spi_suspend,
	.resume         = ce4XXX_spi_resume,
#endif
	.remove         = ce4100_spi_remove,
};

module_pci_driver(ce4100_spi_driver);

MODULE_DESCRIPTION("CE4100 PCI-SPI glue code for PXA's driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sebastian Andrzej Siewior <bigeasy@linutronix.de>");
