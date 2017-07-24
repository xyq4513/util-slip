#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include "github.com/Lobaro/util-ringbuf/drv_ringbuf.h"
#include "github.com/Lobaro/c-utils/lobaroAssert.h"
#include "github.com/Lobaro/c-utils/logging.h"
#include "slip.h"
#include "FreeRTOS.h"
#include "task.h"
#include "fcs16.h"

static SemaphoreHandle_t txSemaphore = NULL;
static SemaphoreHandle_t rxSemaphore = NULL;

static void takeSemaphore(SemaphoreHandle_t sem) {
	if (sem != NULL) {
		if (!xSemaphoreTake(sem, pdMS_TO_TICKS(1000))) {
			lobaroASSERT(false);
		}
	}
}

static void giveSemaphore(SemaphoreHandle_t sem) {
	if (sem != NULL) {
		if (!xSemaphoreGive(sem)) {
			lobaroASSERT(false);
		}
	}
}

void slipmux_setSemaphores(SemaphoreHandle_t rxSem, SemaphoreHandle_t txSem) {
	rxSemaphore = rxSem;
	txSemaphore = txSem;
}

/* SEND_PACKET: sends a packet of length "len", starting at
 * location "p".
 */
void slipmux_send_packet(uint8_t *p, int len, uint8_t type, void (*send_char)(char c)) {
	takeSemaphore(txSemaphore);
	/* send an initial END character to flush out any data that may
	 * have accumulated in the receiver due to line noise
	 */
	send_char(SLIP_END);

	// TODO: Skip type for IP packets
	send_char(type);
	uint16_t fcs = 0;
	if (type == SLIPMUX_COAP) {
		uint8_t frameType = SLIPMUX_COAP;
		fcs = CalcFcs16(&frameType, 1); // Include the frameType into the FCS
		fcs = CalcFcs16WithInit(fcs, p, len);
	}

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

	// Append checksum
	if (type == SLIPMUX_COAP) {
		fcs ^= 0xffff; // Complement
		send_char((uint8_t) fcs); // least significant byte first
		send_char((uint8_t) (fcs >> 8));
	}

	/* tell the receiver that we're done sending the packet
	 */
	send_char(SLIP_END);
	giveSemaphore(txSemaphore);
}

/* RECV_PACKET: reads a packet from buf into the buffer located at "p".
 *      If more than len bytes are received, the packet will
 *      be truncated.
 *      type must be 0 for new packets. If a partial packet was received, set type to that type != 0
 *      type will be set to the type of the packet, which is the first byte in SLIPMUX
 *      Returns the number of bytes stored in the buffer.
 */
int slipmux_read_packet(volatile slipBuffer_t* buf, uint8_t *p, int len, uint8_t* pType) {
	char c;
	int received = 0;
	bool first = (*pType == 0);

	takeSemaphore(rxSemaphore);

	if (buf->packetCnt == 0) {
		giveSemaphore(rxSemaphore);
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
			if (buf->packetCnt != 0) {
				Log("Buffer is empty: start: %d, end: %d - but packetCnt = %d", buf->ringBuf.start,buf->ringBuf.end, buf->packetCnt);
			}
			configASSERT(buf->packetCnt == 0);
			giveSemaphore(rxSemaphore);
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
				taskENTER_CRITICAL();
				buf->packetCnt--;
				taskEXIT_CRITICAL();
				if (*pType == SLIPMUX_COAP && received >= 2) {
					received -= 2; // Remove crc
				}
				giveSemaphore(rxSemaphore);
				return received;
			}
			else {
				// Ignore empty packets
				first = true;
				break;
			}

			/* if it's the same code as an ESC character, wait
			 * and get another character and then figure out
			 * what to store in the packet based on that.
			 */
		case SLIP_ESC:
			if (isBufferEmpty(&(buf->ringBuf))) {
				configASSERT(buf->packetCnt == 0);
				giveSemaphore(rxSemaphore);
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
				if (first) {
					// TODO: Do not skip IPv4 and IPv6 first bytes
					*pType = c;
					first = false;
				} else {
					p[received++] = c;
				}
			}
			break;
		default:
			// Store the character
			if (received < len) {
				if (first) {
					*pType = c;
					first = false;
				} else {
					p[received++] = c;
				}
			}
		}
	}
	giveSemaphore(rxSemaphore);
}
