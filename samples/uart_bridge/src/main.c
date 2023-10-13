#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

struct rx_done_event {
	uint8_t *buf;
	size_t len;
};

K_MSGQ_DEFINE(rx_queue_0, sizeof(struct rx_done_event), 10, 2);
K_MSGQ_DEFINE(rx_queue_1, sizeof(struct rx_done_event), 10, 2);
K_MSGQ_DEFINE(rx_queue_2, sizeof(struct rx_done_event), 10, 2);

/* copying from connectivity bridge */
static const struct device *uart_devices[] = {
	DEVICE_DT_GET(DT_ALIAS(serial_usb)),
	DEVICE_DT_GET(DT_ALIAS(serial_uart)),
};

static struct k_msgq *uart_rx_queues[] = {
	&rx_queue_0,
	&rx_queue_1,
};

#define UART_DEVICE_COUNT ARRAY_SIZE(uart_devices)

#define UART_BUF_SIZE 4096

#define UART_SLAB_BLOCK_SIZE  sizeof(struct uart_rx_buf)
#define UART_SLAB_BLOCK_COUNT (UART_DEVICE_COUNT * 3)
#define UART_SLAB_ALIGNMENT   4
#define UART_RX_TIMEOUT_USEC  1000

#if defined(CONFIG_PM_DEVICE)
#define UART_SET_PM_STATE true
#else
#define UART_SET_PM_STATE false
#endif

struct uart_rx_buf {
	atomic_t ref_counter;
	size_t len;
	uint8_t buf[UART_BUF_SIZE];
};

struct uart_tx_buf {
	struct ring_buf rb;
	uint8_t buf[UART_BUF_SIZE];
};

BUILD_ASSERT((sizeof(struct uart_rx_buf) % UART_SLAB_ALIGNMENT) == 0);

/* Blocks from the same slab is used for RX for all UART instances */
/* TX has inidividual ringbuffers per UART instance */

K_MEM_SLAB_DEFINE(uart_rx_slab, UART_SLAB_BLOCK_SIZE, UART_SLAB_BLOCK_COUNT, UART_SLAB_ALIGNMENT);

static struct uart_tx_buf uart_tx_ringbufs[UART_DEVICE_COUNT];
static uint32_t uart_default_baudrate[UART_DEVICE_COUNT];
/* UART RX only enabled when there is one or more subscribers (power saving) */
static bool enable_rx_retry[UART_DEVICE_COUNT];
static atomic_t uart_tx_started[UART_DEVICE_COUNT];

static void enable_uart_rx(uint8_t dev_idx);
static void disable_uart_rx(uint8_t dev_idx);
static void set_uart_power_state(uint8_t dev_idx, bool active);
static int uart_tx_start(uint8_t dev_idx);
static void uart_tx_finish(uint8_t dev_idx, size_t len);

static inline struct uart_rx_buf *block_start_get(uint8_t *buf)
{
	size_t block_num;

	/* blocks are fixed size units from a continuous memory slab: */
	/* round down to the closest unit size to find beginning of block. */

	block_num = (((size_t)buf - (size_t)uart_rx_slab.buffer) / UART_SLAB_BLOCK_SIZE);

	return (struct uart_rx_buf *)&uart_rx_slab.buffer[block_num * UART_SLAB_BLOCK_SIZE];
}

static struct uart_rx_buf *uart_rx_buf_alloc(void)
{
	struct uart_rx_buf *buf;
	int err;

	/* Async UART driver returns pointers to received data as */
	/* offsets from beginning of RX buffer block. */
	/* This code uses a reference counter to keep track of the number of */
	/* references within a single RX buffer block */

	err = k_mem_slab_alloc(&uart_rx_slab, (void **)&buf, K_NO_WAIT);
	if (err) {
		return NULL;
	}

	atomic_set(&buf->ref_counter, 1);

	return buf;
}

static void uart_rx_buf_ref(void *buf)
{
	__ASSERT_NO_MSG(buf);

	atomic_inc(&(block_start_get(buf)->ref_counter));
}

static void uart_rx_buf_unref(void *buf)
{
	__ASSERT_NO_MSG(buf);

	struct uart_rx_buf *uart_buf = block_start_get(buf);
	atomic_t ref_counter = atomic_dec(&uart_buf->ref_counter);

	/* ref_counter is the uart_buf->ref_counter value prior to decrement */
	if (ref_counter == 1) {
		k_mem_slab_free(&uart_rx_slab, (void **)&uart_buf);
	}
}

static void uart_callback(const struct device *dev, struct uart_event *evt, void *user_data)
{
	int dev_idx = (int)user_data;
	struct uart_rx_buf *buf;
	int err;

	switch (evt->type) {
	case UART_RX_RDY:
		uart_rx_buf_ref(evt->data.rx.buf);

		struct rx_done_event event = {
			.buf = &evt->data.rx.buf[evt->data.rx.offset],
			.len = evt->data.rx.len,
		};

		k_msgq_put(uart_rx_queues[dev_idx], &event, K_NO_WAIT);

		break;
	case UART_RX_BUF_RELEASED:
		if (evt->data.rx_buf.buf) {
			uart_rx_buf_unref(evt->data.rx_buf.buf);
		}
		break;
	case UART_RX_BUF_REQUEST:
		buf = uart_rx_buf_alloc();
		if (buf == NULL) {
			LOG_WRN("UART_%d RX overflow", dev_idx);
			break;
		}

		err = uart_rx_buf_rsp(dev, buf->buf, sizeof(buf->buf));
		if (err) {
			LOG_ERR("uart_rx_buf_rsp: %d", err);
			uart_rx_buf_unref(buf);
		}
		break;
	case UART_RX_DISABLED:
		if (enable_rx_retry[dev_idx]) {
			enable_uart_rx(dev_idx);
			enable_rx_retry[dev_idx] = false;
		} else if (UART_SET_PM_STATE) {
			set_uart_power_state(dev_idx, false);
		}
		break;
	case UART_TX_DONE:
		uart_tx_finish(dev_idx, evt->data.tx.len);

		if (ring_buf_is_empty(&uart_tx_ringbufs[dev_idx].rb)) {
			atomic_set(&uart_tx_started[dev_idx], false);
		} else {
			uart_tx_start(dev_idx);
		}
		break;
	case UART_TX_ABORTED:
		uart_tx_finish(dev_idx, evt->data.tx.len);
		atomic_set(&uart_tx_started[dev_idx], false);
		break;
	case UART_RX_STOPPED:
		LOG_WRN("UART_%d stop reason %d", dev_idx, evt->data.rx_stop.reason);

		/* Retry automatically in case of unexpected stop.
		 * Typically happens when the peer does not drive its TX GPIO,
		 * or if there is a baud rate mismatch.
		 */
		enable_rx_retry[dev_idx] = true;
		break;
	default:
		LOG_ERR("Unexpected event: %d", evt->type);
		__ASSERT_NO_MSG(false);
		break;
	}
}

static void set_uart_baudrate(uint8_t dev_idx, uint32_t baudrate)
{
	const struct device *dev = uart_devices[dev_idx];
	struct uart_config cfg;
	int err;

	if (baudrate == 0) {
		return;
	}

	err = uart_config_get(dev, &cfg);
	if (err) {
		LOG_ERR("uart_config_get: %d", err);
		return;
	}

	if (cfg.baudrate == baudrate) {
		return;
	}

	cfg.baudrate = baudrate;
	err = uart_configure(dev, &cfg);
	if (err) {
		LOG_ERR("uart_configure: %d", err);
		return;
	}
}

static void set_uart_power_state(uint8_t dev_idx, bool active)
{
#if UART_SET_PM_STATE
	const struct device *dev = uart_devices[dev_idx];
	int err;
	enum pm_device_action action;

	action = active ? PM_DEVICE_ACTION_RESUME : PM_DEVICE_ACTION_SUSPEND;

	err = pm_device_action_run(dev, action);
	if ((err < 0) && (err != -EALREADY)) {
		LOG_ERR("pm_device_action_run failed: %d", err);
	}
#endif
}

static void enable_uart_rx(uint8_t dev_idx)
{
	const struct device *dev = uart_devices[dev_idx];
	int err;
	struct uart_rx_buf *buf;

	err = uart_callback_set(dev, uart_callback, (void *)(int)dev_idx);
	if (err) {
		LOG_ERR("uart_callback_set: %d", err);
		return;
	}

	buf = uart_rx_buf_alloc();
	if (!buf) {
		LOG_ERR("uart_rx_buf_alloc error");
		return;
	}

	err = uart_rx_enable(dev, buf->buf, sizeof(buf->buf), UART_RX_TIMEOUT_USEC);
	if (err) {
		uart_rx_buf_unref(buf);
		LOG_ERR("uart_rx_enable: %d", err);
		return;
	}
}

static void disable_uart_rx(uint8_t dev_idx)
{
	const struct device *dev = uart_devices[dev_idx];
	int err;

	err = uart_rx_disable(dev);
	if (err) {
		LOG_ERR("uart_rx_disable: %d", err);
		return;
	}
}

static int uart_tx_start(uint8_t dev_idx)
{
	int len;
	int err;
	uint8_t *buf;

	len = ring_buf_get_claim(&uart_tx_ringbufs[dev_idx].rb, &buf,
				 sizeof(uart_tx_ringbufs[dev_idx].buf));

	err = uart_tx(uart_devices[dev_idx], buf, len, 0);
	if (err) {
		LOG_ERR("uart_tx: %d", err);
		uart_tx_finish(dev_idx, 0);
		return err;
	}

	return 0;
}

static void uart_tx_finish(uint8_t dev_idx, size_t len)
{
	int err;

	err = ring_buf_get_finish(&uart_tx_ringbufs[dev_idx].rb, len);
	if (err) {
		LOG_ERR("ring_buf_get_finish: %d", err);
	}
}

static int uart_tx_enqueue(const uint8_t *data, size_t data_len, uint8_t dev_idx)
{
	atomic_t started;
	uint32_t written;
	int err;

	written = ring_buf_put(&uart_tx_ringbufs[dev_idx].rb, data, data_len);
	if (written == 0) {
		return -ENOMEM;
	}

	started = atomic_set(&uart_tx_started[dev_idx], true);
	if (!started) {
		err = uart_tx_start(dev_idx);
		if (err) {
			LOG_ERR("uart_tx_start: %d", err);
			atomic_set(&uart_tx_started[dev_idx], false);
		}
	}

	if (written == data_len) {
		return 0;
	} else {
		return -ENOMEM;
	}

	return 0;
}

/* end of copycat */

/* USB CDC ACM start */
static const struct device *cdc_devices[] = {
	DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0)),
};
#define CDC_DEVICE_COUNT       ARRAY_SIZE(cdc_devices)
#define USB_CDC_DTR_POLL_MS    500
#define USB_CDC_RX_BLOCK_SIZE  4096
#define USB_CDC_RX_BLOCK_COUNT (CDC_DEVICE_COUNT * 3)
#define USB_CDC_SLAB_ALIGNMENT 4

static void cdc_dtr_timer_handler(struct k_timer *timer);
static void cdc_dtr_work_handler(struct k_work *work);
static K_TIMER_DEFINE(cdc_dtr_timer, cdc_dtr_timer_handler, NULL);
static K_WORK_DEFINE(cdc_dtr_work, cdc_dtr_work_handler);
/* Incoming data from any CDC instance is copied into a block from this slab */
K_MEM_SLAB_DEFINE(cdc_rx_slab, USB_CDC_RX_BLOCK_SIZE, USB_CDC_RX_BLOCK_COUNT,
		  USB_CDC_SLAB_ALIGNMENT);

static uint32_t cdc_ready[CDC_DEVICE_COUNT];
static uint8_t cdc_overflow_buf[64];

static struct k_msgq *cdc_rx_queues[] = {
	&rx_queue_2,
};

static void cdc_dtr_timer_handler(struct k_timer *timer)
{
	k_work_submit(&cdc_dtr_work);
}

/* wait until CDC port has been opened */
static void cdc_poll_dtr(void)
{
	for (int i = 0; i < CDC_DEVICE_COUNT; ++i) {
		int err;
		uint32_t cdc_val;

		err = uart_line_ctrl_get(cdc_devices[i], UART_LINE_CTRL_DTR, &cdc_val);
		if (err) {
			LOG_WRN("uart_line_ctrl_get(DTR): %d", err);
			continue;
		}

		if (cdc_val != cdc_ready[i]) {
			uint32_t baudrate;

			err = uart_line_ctrl_get(cdc_devices[i], UART_LINE_CTRL_BAUD_RATE, &baudrate);
			if (err) {
				LOG_WRN("uart_line_ctrl_get(BAUD_RATE): %d", err);
				continue;
			}

			cdc_ready[i] = cdc_val;
		}
	}
}

static void cdc_dtr_work_handler(struct k_work *work)
{
	cdc_poll_dtr();
}

static void cdc_uart_interrupt_handler(const struct device *dev, void *user_data)
{
	int dev_idx = (int)user_data;

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		void *rx_buf;
		int err;
		int data_length;

		if (cdc_ready[dev_idx] == 0) {
			/* Peer started sending data before timed COM port state poll */
			cdc_poll_dtr();
		}

		err = k_mem_slab_alloc(&cdc_rx_slab, &rx_buf, K_NO_WAIT);
		if (err) {
			data_length = uart_fifo_read(dev, cdc_overflow_buf, sizeof(cdc_overflow_buf));
			LOG_WRN("CDC_%d RX overflow", dev_idx);
		} else {
			data_length = uart_fifo_read(dev, rx_buf, USB_CDC_RX_BLOCK_SIZE);

			if (data_length) {
				struct rx_done_event event = {
					.buf = rx_buf,
					.len = data_length,
				};

				k_msgq_put(cdc_rx_queues[dev_idx], &event, K_NO_WAIT);

			} else {
				k_mem_slab_free(&cdc_rx_slab, &rx_buf);
			}
		}
		// TODO: disable RX when mem slab is exhausted
	}
}

static void cdc_enable_rx_irq(int dev_idx)
{
	uart_irq_callback_user_data_set(cdc_devices[dev_idx], cdc_uart_interrupt_handler,
					(void *)dev_idx);
	uart_irq_rx_enable(cdc_devices[dev_idx]);
}


static int cdc_tx_enqueue(const uint8_t *data, size_t data_len, uint8_t dev_idx)
{

	if (dev_idx >= CDC_DEVICE_COUNT) {
		return false;
	}

	/* this is not officially allowed to be called outside ISRs.
	   we'll do it anyway for we are savages. */
	int written = uart_fifo_fill(
		cdc_devices[dev_idx],
		data,
		data_len
	);

	if (written == data_len) {
		return 0;
	} else {
		return -ENOMEM;
	}
}

static void usbd_status(enum usb_dc_status_code cb_status, const uint8_t *param)
{

	switch (cb_status) {
	case USB_DC_ERROR:
		LOG_ERR("USB_DC_ERROR");
		break;
	case USB_DC_RESET:
		LOG_DBG("USB_DC_RESET");
		break;
	case USB_DC_CONNECTED:
		LOG_DBG("USB_DC_CONNECTED");
		break;
	case USB_DC_CONFIGURED:
		LOG_DBG("USB_DC_CONFIGURED");
		k_timer_start(&cdc_dtr_timer, K_MSEC(USB_CDC_DTR_POLL_MS), K_MSEC(USB_CDC_DTR_POLL_MS));
		break;
	case USB_DC_DISCONNECTED:
		LOG_DBG("USB_DC_DISCONNECTED");
		k_timer_stop(&cdc_dtr_timer);
		break;
	case USB_DC_SUSPEND:
		LOG_DBG("USB_DC_SUSPEND");
		k_timer_stop(&cdc_dtr_timer);
		break;
	case USB_DC_RESUME:
		LOG_DBG("USB_DC_RESUME");
		break;
	default:
		break;
	}
}

/* USB CDC ACM end */

int main(void)
{
	int err;

	struct k_poll_event poll_evts[3];

	k_poll_event_init(&poll_evts[0], K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &rx_queue_0);
	k_poll_event_init(&poll_evts[1], K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &rx_queue_1);
	k_poll_event_init(&poll_evts[2], K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &rx_queue_2);

	LOG_INF("Hello World! %s", CONFIG_BOARD);

	/* initialize UART devices */
	for (int i = 0; i < UART_DEVICE_COUNT; ++i) {
		struct uart_config cfg;
		if (!device_is_ready(uart_devices[i])) {
			LOG_ERR("UART device not ready: %s", uart_devices[i]->name);
			continue;
		}
		err = uart_config_get(uart_devices[i], &cfg);
		if (err) {
			LOG_ERR("uart_config_get: %d", err);
			return false;
		}
		uart_default_baudrate[i] = cfg.baudrate;
		enable_rx_retry[i] = false;
		atomic_set(&uart_tx_started[i], false);
		ring_buf_init(&uart_tx_ringbufs[i].rb, sizeof(uart_tx_ringbufs[i].buf),
			      uart_tx_ringbufs[i].buf);

		if (UART_SET_PM_STATE) {
			set_uart_power_state(i, true);
		}
		enable_uart_rx(i);
	}

	/* initialize USB CDC */
	err = usb_enable(NULL);
	if (err) {
		LOG_ERR("usb_enable: %d", err);
		return false;
	}
	for (int i = 0; i < CDC_DEVICE_COUNT; ++i) {
		cdc_ready[i] = 0;
		if (device_is_ready(cdc_devices[i])) {
			cdc_enable_rx_irq(i);
			LOG_DBG("%s available", cdc_devices[i]->name);
		} else {
			LOG_ERR("%s not available", cdc_devices[i]->name);
		}
	}

	/* event loop */
	while (true) {
		struct rx_done_event event = {0};

		for (int i = 0; i < UART_DEVICE_COUNT; ++i) {
			if (!k_msgq_get(uart_rx_queues[i], (void *)&event, K_NO_WAIT)) {
				LOG_INF("UART%d->USB: [%.*s]", i, event.len, event.buf);
				cdc_tx_enqueue(event.buf, event.len, 0);
				uart_rx_buf_unref(event.buf);
			}
		}
		if (!k_msgq_get(cdc_rx_queues[0], (void *)&event, K_NO_WAIT)) {
			LOG_INF("USB->UART0: [%.*s]", event.len, event.buf);
			uart_tx_enqueue(event.buf, event.len, 0);
			k_mem_slab_free(&cdc_rx_slab, (void **) &event.buf);
		}
		k_poll(&poll_evts, 3, K_FOREVER);
	}

	return 0;
}
