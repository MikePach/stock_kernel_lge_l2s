#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/pm.h>

#include <linux/spi/spi.h>         /* HOST Interface SPI */
#include <linux/spi/spidev.h>   /* HOST Interface SPI */
#include <linux/interrupt.h>        /* HOST Interface SPI */

#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/wakelock.h> 		/* wake_lock, unlock */

#include "../../broadcast_tdmb_drv_ifdef.h"
#include "../inc/broadcast_t3900.h"

#include <linux/pm_qos_params.h>	/* PM QOS */
#include CONFIG_BOARD_HEADER_FILE
#include <mach/board_lge.h>			/* To use lge_get_board_revno() for clock sharing in HW_REV_C/HW_REV_D */
#include <mach/msm_xo.h>

/* T3900 driver contrl block */
struct tdmb_t3900_ctrl_blk 
{
	int 					is_power_on;
	struct spi_device* 			spi_ptr;
	struct work_struct 			spi_work;
	struct workqueue_struct* 		spi_wq;
	struct mutex				mutex;
	struct wake_lock 			wake_lock;	/* wake_lock,wake_unlock */
	boolean 				irq_status;
	spinlock_t				spin_lock;
	struct pm_qos_request_list      pm_req_list;     
};

static struct tdmb_t3900_ctrl_blk t3900_ctrl_info;


// -------------------------------------------------------------------- 
// D1L DMB GPIOs
#define T3900_DMB_INT_N           75	/* IRQ, IN, LOW_ACTIVE */			
#define T3900_DMB_RESET_N         94     /* RESET, OUT, LOW_ACTIVE */
#define T3900_DMB_EN              98    /* PWR_EN, OUT, HIGH_ACTIVE */
/*
* Extended GPIOs from PMIC8921
* Refer to kernel/arch/arm/mach-msm/lge/board_d1l_pm.c for registration.
*/
#define DMB_ANT_SEL_EAR	                (PM8921_GPIO_PM_TO_SYS(16))
#define DMB_ANT_SEL_INN	                (PM8921_GPIO_PM_TO_SYS(17))
//---------------------------------------------------------------------

static uint32 user_stop_flg = 0;
static const char *id = "TDMB";
static struct msm_xo_voter *xo_handle_tdmb;

/* ============================================================== */
/*  Internal Functions                                                                                                                */
/* ============================================================== */
static int broadcast_t3900_probe(struct spi_device *spi);
static int broadcast_t3900_remove(struct spi_device *spi);
static int broadcast_t3900_suspend(struct spi_device *spi, pm_message_t mesg);
static int broadcast_t3900_resume(struct spi_device *spi);

void tdmb_t3900_set_userstop(int mode)
{
	user_stop_flg = mode;
}

int tdmb_t3900_mdelay(int32 ms)
{
	int		rc = 1;  /* 0 : false, 1 : ture */
	int32	wait_loop =0;
	int32	wait_ms = ms;

	if(ms > 100)
	{
		wait_loop = (ms /100);   /* 100, 200, 300 more only , Otherwise this must be modified e.g (ms + 40)/50 */
		wait_ms = 100;
	}

	do
	{
		mdelay(wait_ms);
		if(user_stop_flg == 1)
		{
			printk("~~~~~~~~ Ustop flag is set so return false ms =(%d)~~~~~~~\n", ms);
			rc = 0;
			break;
		}
	}while((--wait_loop) > 0);

	return rc;
}

void tdmb_t3900_must_mdelay(int32 ms)
{
	mdelay(ms);
}

int tdmb_t3900_power_on(void)
{
	int rc;

	if ( t3900_ctrl_info.is_power_on == FALSE )
	{
		/* QOS */
		if(pm_qos_request_active(&t3900_ctrl_info.pm_req_list)) {
			pm_qos_update_request(&t3900_ctrl_info.pm_req_list, 20);	
		}
		wake_lock(&t3900_ctrl_info.wake_lock);
		
		if (lge_get_board_revno() == HW_REV_C || lge_get_board_revno() == HW_REV_D) {
			rc = msm_xo_mode_vote(xo_handle_tdmb, MSM_XO_MODE_ON);
			if(rc < 0) {
				pr_err("Configuring MSM_XO_MODE_ON failed (%d)\n", rc);
				msm_xo_put(xo_handle_tdmb);
				return FALSE;
			}
		}

		gpio_set_value_cansleep(DMB_ANT_SEL_EAR, 0);
		gpio_set_value_cansleep(DMB_ANT_SEL_INN, 1);

		/* T3900 Power On Sequence */
		gpio_set_value(T3900_DMB_EN, 0);
		gpio_set_value(T3900_DMB_RESET_N, 0);
		udelay(200);
		
		gpio_set_value(T3900_DMB_EN, 1);
		mdelay(3);

		gpio_set_value(T3900_DMB_RESET_N, 1);
		mdelay(2);

		gpio_set_value(T3900_DMB_RESET_N, 0);
		mdelay(1);

		gpio_set_value(T3900_DMB_RESET_N, 1);
		udelay(200);

		tdmb_t3900_interrupt_free();
		t3900_ctrl_info.is_power_on = TRUE;

	}
	else
	{
		printk("tdmb_t3900_power_on the power already turn on \n");
	}

	printk("tdmb_t3900_power_on completed \n");

	return TRUE;

}

int tdmb_t3900_power_off(void)
{
	if ( t3900_ctrl_info.is_power_on == TRUE )
	{
		if (lge_get_board_revno() == HW_REV_C || lge_get_board_revno() == HW_REV_D) {
			if(xo_handle_tdmb != NULL) {
				msm_xo_mode_vote(xo_handle_tdmb, MSM_XO_MODE_OFF);
			}
		}

		tdmb_t3900_interrupt_lock();
		t3900_ctrl_info.is_power_on = FALSE;

		gpio_set_value(T3900_DMB_RESET_N, 0);
		udelay(100);

		gpio_set_value(T3900_DMB_EN, 0);
		udelay(500);

        gpio_set_value_cansleep(DMB_ANT_SEL_EAR, 1); // For ESD Test(from i-Pjt)
        gpio_set_value_cansleep(DMB_ANT_SEL_INN, 0);

		wake_unlock(&t3900_ctrl_info.wake_lock);

		/* QoS release */
		if(pm_qos_request_active(&t3900_ctrl_info.pm_req_list)) {
			pm_qos_update_request(&t3900_ctrl_info.pm_req_list, PM_QOS_DEFAULT_VALUE);	
		}
	}
	else
	{
		printk("tdmb_t3900_power_on the power already turn off \n");
	}	

	printk("tdmb_t3900_power_off completed \n");
		
	return TRUE;
}

int tdmb_t3900_select_antenna(unsigned int sel)
{
    if(LGE_BROADCAST_TDMB_ANT_TYPE_INTENNA == sel) {
        gpio_set_value_cansleep(DMB_ANT_SEL_EAR, 0);
        gpio_set_value_cansleep(DMB_ANT_SEL_INN, 1);
        return TRUE;
    }
    else if(LGE_BROADCAST_TDMB_ANT_TYPE_EARANT == sel) {
    
        gpio_set_value_cansleep(DMB_ANT_SEL_EAR, 1);
        gpio_set_value_cansleep(DMB_ANT_SEL_INN, 0);
        return TRUE;
    }
	return FALSE;
}

static struct spi_driver broadcast_tdmb_driver = {
	.probe = broadcast_t3900_probe,
	.remove = __devexit_p(broadcast_t3900_remove),
	.suspend = broadcast_t3900_suspend,
	.resume  = broadcast_t3900_resume,
	.driver = {
		.name = "tdmb_t3900",
		.bus	= &spi_bus_type,
		.owner = THIS_MODULE,
	},
};

void tdmb_t3900_interrupt_lock(void)
{
	if (t3900_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_t3900_interrupt_lock fail\n");
		return;
	}

	disable_irq(t3900_ctrl_info.spi_ptr->irq);
	return;
}

void tdmb_t3900_interrupt_free(void)
{
	if (t3900_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_t3900_interrupt_free fail\n");
		return;
	}

	enable_irq(t3900_ctrl_info.spi_ptr->irq);
	return;
}


int tdmb_t3900_spi_write_read(uint8* tx_data, int tx_length, uint8 *rx_data, int rx_length)
{
	int rc;
	
	struct spi_transfer	t = {
			.tx_buf		= tx_data,
			.rx_buf		= rx_data,
			.len			= tx_length+rx_length,
		};
	struct spi_message	m;	

	//printk("tdmb_t3900_spi_write_read start\n");

	if (t3900_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_t3900_spi_write_read error txdata=0x%x, length=%d\n", (unsigned int)tx_data, tx_length+rx_length);
		return FALSE;
	}

	mutex_lock(&t3900_ctrl_info.mutex);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	rc = spi_sync(t3900_ctrl_info.spi_ptr, &m);
	//printk("rc = %d in tdmb_t3900_spi_write_read\n", rc);

	if ( rc < 0 )
	{
		printk("tdmb_t3900_spi_read_burst result(%d), actual_len=%d\n",rc, m.actual_length);
	}

	mutex_unlock(&t3900_ctrl_info.mutex);


	return TRUE;
}


#if 0
void tdmb_rw_test(void)
{
	unsigned short i = 0;
	unsigned short w_val = 0;
	unsigned short r_val = 0;
	unsigned short err_cnt = 0;

	err_cnt = 0;
	for(i=1;i<11;i++)
	{
		w_val = (i%0xFF);
		tdmb_t3900_spi_write_read( 0x0a00+ 0x05, w_val, 0x0a00+ 0x05, &r_val );
		//tdmb_t3900_i2c_read16(0x0a00+ 0x05, &r_val );
		if(r_val != w_val)
		{
			err_cnt++;
			printk("w_val:%x, r_val:%x\n", w_val,r_val);
		}
	}
}
#endif

static irqreturn_t broadcast_tdmb_spi_isr(int irq, void *handle)
{
	struct tdmb_t3900_ctrl_blk* t3900_info_p;
	unsigned long flag;

	t3900_info_p = (struct tdmb_t3900_ctrl_blk *)handle;	
	if ( t3900_info_p && t3900_info_p->is_power_on )
	{
		if (t3900_info_p->irq_status)
		{			
			printk("######### T3900 spi read funtion is so late skip #########\n");			
			return IRQ_HANDLED;
		}
		spin_lock_irqsave(&t3900_info_p->spin_lock, flag);
		queue_work(t3900_info_p->spi_wq, &t3900_info_p->spi_work);
		spin_unlock_irqrestore(&t3900_info_p->spin_lock, flag);
	}
	else
	{
		printk("broadcast_tdmb_spi_isr is called, but device is off state\n");
	}

	return IRQ_HANDLED; 
}

static void broacast_tdmb_spi_work(struct work_struct *tdmb_work)
{
	struct tdmb_t3900_ctrl_blk *t3900_info_p;

	t3900_info_p = container_of(tdmb_work, struct tdmb_t3900_ctrl_blk, spi_work);
	if ( t3900_info_p )
	{
		t3900_info_p->irq_status = TRUE;
		broadcast_drv_if_read_data();
		t3900_info_p->irq_status = FALSE;
	}
}

static int tdmb_t3900_configure_gpios(void)
{
	int rc= 0;

	rc = gpio_request(T3900_DMB_EN, "dmb_enable");
	if(rc)
	{
		pr_warning("T3900 DMB_EN gpio request error\n");
	}
	gpio_direction_output(T3900_DMB_EN, 0);  /* output and low */

	rc = gpio_request(T3900_DMB_RESET_N, "dmb_reset");
	if(rc)
	{
		pr_warning("T3900 DMB_RESET gpio request error\n");
	}
	gpio_direction_output(T3900_DMB_RESET_N, 0);  /* output and low */

	rc = gpio_request(T3900_DMB_INT_N, "dmb_irq");
	if(rc)
	{
		pr_warning("T3900 DMB_INT_N gpio request error\n");
	}
	gpio_direction_input(T3900_DMB_INT_N);

	return 0;
}


static int  broadcast_t3900_probe(struct spi_device *spi)
{
	int rc;

	t3900_ctrl_info.spi_ptr 				= spi;
	t3900_ctrl_info.spi_ptr->mode 			= SPI_MODE_0;
	t3900_ctrl_info.spi_ptr->bits_per_word 	= 8;
	t3900_ctrl_info.spi_ptr->max_speed_hz 	= (5400*1000);

	rc = spi_setup(spi);
	printk("%s is called and spi_setup\n", "broadcast_t3900_probe");

	INIT_WORK(&t3900_ctrl_info.spi_work, broacast_tdmb_spi_work);

	t3900_ctrl_info.spi_wq = create_singlethread_workqueue("tdmb_spi_wq");
	// t3900_ctrl_info.spi_wq = create_rt_workqueue("tdmb_spi_wq");
	if(t3900_ctrl_info.spi_wq == NULL){
		printk("Failed to setup tdmb spi workqueue \n");
		return -ENOMEM;
	}

	tdmb_t3900_configure_gpios( );
	
	rc = request_irq(spi->irq, broadcast_tdmb_spi_isr, IRQF_DISABLED | IRQF_TRIGGER_FALLING, spi->dev.driver->name, &t3900_ctrl_info);
	printk("broadcast_t3900_probe request_irq=%d\n", rc);

    gpio_set_value_cansleep(DMB_ANT_SEL_EAR, 1); // For ESD Test(from i-Pjt)
    gpio_set_value_cansleep(DMB_ANT_SEL_INN, 0);

	if (lge_get_board_revno() == HW_REV_C || lge_get_board_revno() == HW_REV_D) {
		xo_handle_tdmb = msm_xo_get(MSM_XO_TCXO_D1, id);
		if(IS_ERR(xo_handle_tdmb)) {
			pr_err("Failed to get MSM_XO_TCXO_D1 handle for TDMB (%ld)\n", PTR_ERR(xo_handle_tdmb));
			return FALSE;
		}
	}

	tdmb_t3900_interrupt_lock();

	mutex_init(&t3900_ctrl_info.mutex);

	wake_lock_init(&t3900_ctrl_info.wake_lock,  WAKE_LOCK_SUSPEND, dev_name(&spi->dev));

	spin_lock_init(&t3900_ctrl_info.spin_lock);

	pm_qos_add_request(&t3900_ctrl_info.pm_req_list, PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
	printk("broadcast_t3900_probe End. \n");
	
	return rc;
}

static int broadcast_t3900_remove(struct spi_device *spi)
{
	if (t3900_ctrl_info.spi_wq)
	{
		flush_workqueue(t3900_ctrl_info.spi_wq);
		destroy_workqueue(t3900_ctrl_info.spi_wq);
	}

	free_irq(spi->irq, &t3900_ctrl_info);
	mutex_destroy(&t3900_ctrl_info.mutex);
	wake_lock_destroy(&t3900_ctrl_info.wake_lock);

	pm_qos_remove_request(&t3900_ctrl_info.pm_req_list);
	memset((unsigned char*)&t3900_ctrl_info, 0x0, sizeof(struct tdmb_t3900_ctrl_blk));
	return 0;
}


static int broadcast_t3900_suspend(struct spi_device *spi, pm_message_t mesg)
{
	printk("broadcast_t3900_suspend \n");
	return 0;
}

static int broadcast_t3900_resume(struct spi_device *spi)
{
	printk("broadcast_t3900_resume \n");
	return 0;
}

int __devinit broadcast_tdmb_drv_init(void)
{
	int rc;
	//printk("broadcast_tdmb_drv_init\n");
	printk("%s is called\n", "broadcast_tdmb_drv_init");
	rc = broadcast_tdmb_drv_start();
	
	if (rc) {
		printk("broadcast_tdmb_drv_start %s failed to load\n", __func__);
		return rc;
	}
	rc = spi_register_driver(&broadcast_tdmb_driver);
	printk("broadcast_spi_add_driver rc = (%d)\n", rc);

	return rc;
}

static void __exit broadcast_tdmb_drv_exit(void)
{
	spi_unregister_driver(&broadcast_tdmb_driver);
}


/* EXPORT_SYMBOL() : when we use external symbol 
which is not included in current module - over kernel 2.6 */
//EXPORT_SYMBOL(broadcast_tdmb_is_on);

module_init(broadcast_tdmb_drv_init);
module_exit(broadcast_tdmb_drv_exit);
MODULE_DESCRIPTION("broadcast_tdmb_drv_init");
MODULE_LICENSE("INC");
