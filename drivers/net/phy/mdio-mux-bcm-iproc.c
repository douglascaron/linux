/*
 * Copyright 2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation (the "GPL").
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 (GPLv2) for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 (GPLv2) along with this source code.
 */
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/of_mdio.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/mdio-mux.h>
#include <linux/delay.h>
#include <linux/debugfs.h>

#define MDIO_RATE_ADJ_EXT_OFFSET	0x000
#define MDIO_RATE_ADJ_INT_OFFSET	0x004
#define MDIO_RATE_ADJ_DIVIDENT_SHIFT	16

#define MDIO_PARAM_OFFSET		0x23c
#define MDIO_PARAM_MIIM_CYCLE		29
#define MDIO_PARAM_INTERNAL_SEL		25
#define MDIO_PARAM_BUS_ID		22
#define MDIO_PARAM_C45_SEL		21
#define MDIO_PARAM_PHY_ID		16
#define MDIO_PARAM_PHY_DATA		0

#define MDIO_READ_OFFSET		0x240
#define MDIO_READ_DATA_MASK		0xffff
#define MDIO_ADDR_OFFSET		0x244

#define MDIO_CTRL_OFFSET		0x248
#define MDIO_CTRL_WRITE_OP		0x1
#define MDIO_CTRL_READ_OP		0x2

#define MDIO_STAT_OFFSET		0x24c
#define MDIO_STAT_DONE			1

#define BUS_MAX_ADDR			32
#define EXT_BUS_START_ADDR		16

#define MDIO_OPERATING_FREQUENCY	11000000
#define MDIO_RATE_ADJ_DIVIDENT		1

struct iproc_mdiomux_desc {
	void *mux_handle;
	void __iomem *base;
	struct device *dev;
	struct mii_bus *mii_bus;
	struct dentry *dentry_mux;
	struct clk *core_clk;
};

static void mdio_mux_iproc_config_clk(struct iproc_mdiomux_desc *md)
{
	u32 val;
	u32 divisor;

	if (md->core_clk) {
		divisor = clk_get_rate(md->core_clk) / MDIO_OPERATING_FREQUENCY;
		divisor = divisor / (MDIO_RATE_ADJ_DIVIDENT + 1);
		val = divisor;
		val |= MDIO_RATE_ADJ_DIVIDENT << MDIO_RATE_ADJ_DIVIDENT_SHIFT;
		writel(val, md->base + MDIO_RATE_ADJ_EXT_OFFSET);
		writel(val, md->base + MDIO_RATE_ADJ_INT_OFFSET);
	}
}

static int iproc_mdio_wait_for_idle(void __iomem *base, bool result)
{
	unsigned int timeout = 1000; /* loop for 1s */
	u32 val;

	do {
		val = readl(base + MDIO_STAT_OFFSET);
		if ((val & MDIO_STAT_DONE) == result)
			return 0;

		usleep_range(1000, 2000);
	} while (timeout--);

	return -ETIMEDOUT;
}

/* start_miim_ops- Program and start MDIO transaction over mdio bus.
 * @base: Base address
 * @phyid: phyid of the selected bus.
 * @reg: register offset to be read/written.
 * @val :0 if read op else value to be written in @reg;
 * @op: Operation that need to be carried out.
 *      MDIO_CTRL_READ_OP: Read transaction.
 *      MDIO_CTRL_WRITE_OP: Write transaction.
 *
 * Return value: Successful Read operation returns read reg values and write
 *      operation returns 0. Failure operation returns negative error code.
 */
static int start_miim_ops(void __iomem *base,
			  u16 phyid, u32 reg, u16 val, u32 op)
{
	u32 param;
	int ret;

	writel(0, base + MDIO_CTRL_OFFSET);
	ret = iproc_mdio_wait_for_idle(base, 0);
	if (ret)
		goto err;

	param = readl(base + MDIO_PARAM_OFFSET);
	param |= phyid << MDIO_PARAM_PHY_ID;
	param |= val << MDIO_PARAM_PHY_DATA;
	if (reg & MII_ADDR_C45)
		param |= BIT(MDIO_PARAM_C45_SEL);

	writel(param, base + MDIO_PARAM_OFFSET);

	writel(reg, base + MDIO_ADDR_OFFSET);

	writel(op, base + MDIO_CTRL_OFFSET);

	ret = iproc_mdio_wait_for_idle(base, 1);
	if (ret)
		goto err;

	if (op == MDIO_CTRL_READ_OP)
		ret = readl(base + MDIO_READ_OFFSET) & MDIO_READ_DATA_MASK;
err:
	return ret;
}

static int iproc_mdiomux_read(struct mii_bus *bus, int phyid, int reg)
{
	struct iproc_mdiomux_desc *md = bus->priv;
	int ret;

	ret = start_miim_ops(md->base, phyid, reg, 0, MDIO_CTRL_READ_OP);
	if (ret < 0)
		dev_err(&bus->dev, "mdiomux read operation failed!!!");

	return ret;
}

static int iproc_mdiomux_write(struct mii_bus *bus,
			       int phyid, int reg, u16 val)
{
	struct iproc_mdiomux_desc *md = bus->priv;
	int ret;

	/* Write val at reg offset */
	ret = start_miim_ops(md->base, phyid, reg, val, MDIO_CTRL_WRITE_OP);
	if (ret < 0)
		dev_err(&bus->dev, "mdiomux write operation failed!!!");

	return ret;
}

static int mdio_mux_iproc_switch_fn(int current_child, int desired_child,
				    void *data)
{
	struct iproc_mdiomux_desc *md = data;
	u32 param, bus_id;
	bool bus_dir;

	/* select bus and its properties */
	bus_dir = (desired_child < EXT_BUS_START_ADDR);
	bus_id = bus_dir ? desired_child : (desired_child - EXT_BUS_START_ADDR);

	param = (bus_dir ? 1 : 0) << MDIO_PARAM_INTERNAL_SEL;
	param |= (bus_id << MDIO_PARAM_BUS_ID);

	writel(param, md->base + MDIO_PARAM_OFFSET);
	return 0;
}

static ssize_t mux_write(struct file *file, const char __user *user_buf,
			 size_t count, loff_t *ppos)
{
	u16 busid, phyid, reg, val, opt;
	struct iproc_mdiomux_desc *md;
	char *start, *s, buf[32];
	u32 buf_size;
	int ret;

	md = file->f_inode->i_private;

	buf_size = min(count, (size_t)(sizeof(buf) - 1));
	if (copy_from_user(buf, user_buf, buf_size)) {
		dev_err(md->dev, "Failed to copy from user\n");
		return -EFAULT;
	}

	buf[buf_size] = 0;
	dev_err(md->dev, "buffer :%s\n", buf);
	start = buf;

	s = strsep(&start, " ");
	if (!s || !*s)
		return -EINVAL;
	ret = kstrtou16(s, 0, &busid);
	if (ret)
		return -EINVAL;
	dev_info(md->dev, "busid:0x%x\n", (busid < EXT_BUS_START_ADDR) ?
		 busid : busid - EXT_BUS_START_ADDR);

	s = strsep(&start, " ");
	if (!s || !*s)
		return -EINVAL;
	ret = kstrtou16(s, 0, &phyid);
	if (ret)
		return -EINVAL;
	dev_info(md->dev, "phyid:0x%x\n", phyid);

	s = strsep(&start, " ");
	if (!s || !*s)
		return -EINVAL;
	ret = kstrtou16(s, 0, &reg);
	if (ret)
		return -EINVAL;
	dev_info(md->dev, "reg:0x%x\n", reg);

	s = strsep(&start, " ");
	if (!s || !*s)
		return -EINVAL;
	ret = kstrtou16(s, 0, &opt);
	if (ret)
		return -EINVAL;
	dev_info(md->dev, "opt:%s\n", opt ? "write" : "read");

	if (opt) {
		s = strsep(&start, " ");
		if (!s || !*s)
			return -EINVAL;
		ret = kstrtou16(s, 0, &val);
		if (ret)
			return -EINVAL;
		dev_info(md->dev, "val:0x%x\n", val);
	}

	mdio_mux_iproc_switch_fn(0, busid, md);
	if (opt) {
		md->mii_bus->write(md->mii_bus, phyid, reg, val);
	} else {
		ret = md->mii_bus->read(md->mii_bus, phyid, reg);
		dev_info(md->dev, "regval :%x\n", ret);
	}

	return buf_size;
}

static const struct file_operations mux_fops = {
	.write = mux_write,
};

static int mdio_mux_iproc_probe(struct platform_device *pdev)
{
	struct iproc_mdiomux_desc *md;
	struct mii_bus *bus;
	struct resource *res;
	int rc;

	md = devm_kzalloc(&pdev->dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	md->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(md->base)) {
		dev_err(&pdev->dev, "failed to ioremap register\n");
		return PTR_ERR(md->base);
	}

	md->core_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(md->core_clk)) {
		dev_info(&pdev->dev, "core_clk not specified\n");
		md->core_clk = NULL;
	}

	md->mii_bus = mdiobus_alloc();
	if (!md->mii_bus) {
		dev_err(&pdev->dev, "mdiomux bus alloc failed\n");
		return -ENOMEM;
	}

	bus = md->mii_bus;
	bus->priv = md;
	bus->name = "iProc MDIO mux bus";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-%d", pdev->name, pdev->id);
	bus->parent = &pdev->dev;
	bus->read = iproc_mdiomux_read;
	bus->write = iproc_mdiomux_write;

	bus->phy_mask = ~0;
	bus->dev.of_node = pdev->dev.of_node;
	rc = mdiobus_register(bus);
	if (rc) {
		dev_err(&pdev->dev, "mdiomux registration failed\n");
		goto out;
	}

	platform_set_drvdata(pdev, md);

	rc = mdio_mux_init(md->dev, md->dev->of_node, mdio_mux_iproc_switch_fn,
			   &md->mux_handle, md, md->mii_bus);
	if (rc) {
		dev_info(md->dev, "mdiomux initialization failed\n");
		goto out_register;
	}

	mdio_mux_iproc_config_clk(md);

	md->dentry_mux = debugfs_create_file("bcmmux", 0644, NULL,
							md, &mux_fops);
	if (!md->dentry_mux)
		dev_warn(md->dev, "Failed to create debugfs bcmmux file\n");

	dev_info(md->dev, "iProc mdiomux registered\n");
	return 0;

out_register:
	mdiobus_unregister(bus);
out:
	mdiobus_free(bus);
	return rc;
}

static int mdio_mux_iproc_remove(struct platform_device *pdev)
{
	struct iproc_mdiomux_desc *md = dev_get_platdata(&pdev->dev);

	mdio_mux_uninit(md->mux_handle);
	mdiobus_unregister(md->mii_bus);
	debugfs_remove(md->dentry_mux);
	mdiobus_free(md->mii_bus);

	return 0;
}

static const struct of_device_id mdio_mux_iproc_match[] = {
	{
		.compatible = "brcm,mdio-mux-iproc",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mdio_mux_iproc_match);

static struct platform_driver mdiomux_iproc_driver = {
	.driver = {
		.name		= "mdio-mux-iproc",
		.of_match_table = mdio_mux_iproc_match,
	},
	.probe		= mdio_mux_iproc_probe,
	.remove		= mdio_mux_iproc_remove,
};

module_platform_driver(mdiomux_iproc_driver);

MODULE_DESCRIPTION("iProc MDIO Mux Bus Driver");
MODULE_AUTHOR("Pramod Kumar <pramod.kumar@broadcom.com>");
MODULE_LICENSE("GPL v2");
