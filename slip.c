#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include "github.com/Lobaro/util-ringbuf/drv_ringbuf.h"
#include "slip.h"
#include "FreeRTOS.h"
#include "task.h"

void init_slip_buffer(slipBuffer_t* slip_buf, uint8_t* buf, int size) {
	slip_buf->packetCnt = 0;
	slip_buf->last = SLIP_END;
	drv_rbuf_init(&(slip_buf->ringBuf), size, char, buf);
}

// Takes a slip encoded byte from the UART and puts it to the buffer
void slip_uart_putc(volatile slipBuffer_t* slip_buf, char c) {
	configASSERT(!isBufferFull(&(slip_buf->ringBuf)));

	drv_rbuf_write(&(slip_buf->ringBuf), c);
	if (c == SLIP_END && slip_buf->last != SLIP_END) {
		// Got END for non empty packet
		slip_buf->packetCnt++;
	}
	slip_buf->last = c;
}

/* SEND_PACKET: sends a packet of length "len", starting at
 * location "p".
 */
void slip_send_packet(char *p, int len, void (*send_char)(char c)) {

	/* send an initial END character to flush out any data that may
	 * have accumulated in the receiver due to line noise
	 */
	send_char(SLIP_END);

	/* for each byte in the packet, send the appropriate character
	 * sequence
	 */
	while (len--) {
		switch (*p) {
		/* if it's the same code as an END character, we send a
		 * special two character code so as not to make the
		 * receiver think we sent an END
		 */
		case SLIP_END:
			send_char(SLIP_ESC);
			send_char(SLIP_ESC_END);
			break;

			/* if it's the same code as an ESC character,
			 * we send a special two character code so as not
			 * to make the receiver think we sent an ESC
			 */
		case SLIP_ESC:
			send_char(SLIP_ESC);
			send_char(SLIP_ESC_ESC);
			break;
			/* otherwise, we just send the character
			 */
		default:
			send_char(*p);
		}

		p++;
	}

	/* tell the receiver that we're done sending the packet
	 */
	send_char(SLIP_END);
}

/* RECV_PACKET: reads a packet from buf into the buffer located at "p".
 *      If more than len bytes are received, the packet will
 *      be truncated.
 *      Returns the number of bytes stored in the buffer.
 */
int slip_read_packet(volatile slipBuffer_t* buf, uint8_t *p, int len) {
	char c;
	int received = 0;

	if (buf->packetCnt == 0) {
		return 0;
	}

	/* sit in a loop reading bytes until we put together
	 * a whole packet.
	 * Make sure not to copy them into the packet if we
	 * run out of room.
	 */
	while (1) {
		/* get a character to process
		 */
		if (isBufferEmpty(&(buf->ringBuf))) {
			configASSERT(buf->packetCnt == 0);
			return received;
		}
		//taskENTER_CRITICAL();
		configASSERT(!isBufferFull(&(buf->ringBuf)));
		drv_rbuf_read(&(buf->ringBuf), &c);
		//taskEXIT_CRITICAL();

		/* handle bytestuffing if necessary
		 */
		switch (c) {

		/* if it's an END character then we're done with
		 * the packet
		 */
		case SLIP_END:
			/* a minor optimization: if there is no
			 * data in the packet, ignore it. This is
			 * meant to avoid bothering IP with all
			 * the empty packets generated by the
			 * duplicate END characters which are in
			 * turn sent to try to detect line noise.
			 */
			if (received) {
				buf->packetCnt--;
				return received;
			}
			else {
				break;
			}

			/* if it's the same code as an ESC character, wait
			 * and get another character and then figure out
			 * what to store in the packet based on that.
			 */
		case SLIP_ESC:
			if (isBufferEmpty(&(buf->ringBuf))) {
				configASSERT(buf->packetCnt == 0);
				return received;
			}
			//taskENTER_CRITICAL();
			configASSERT(!isBufferFull(&(buf->ringBuf)));
			drv_rbuf_read(&(buf->ringBuf), &c);
			//taskEXIT_CRITICAL();

			/* if "c" is not one of these two, then we
			 * have a protocol violation.  The best bet
			 * seems to be to leave the byte alone and
			 * just stuff it into the packet
			 */
			switch (c) {
			case SLIP_ESC_END:
				c = SLIP_END;
				break;
			case SLIP_ESC_ESC:
				c = SLIP_ESC;
				break;
			}
			// Store the character
			if (received < len) {
				p[received++] = c;
			}
			break;
		default:
			// Store the character
			if (received < len) {
				p[received++] = c;
			}
		}
	}
}
