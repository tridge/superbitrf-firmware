/*
 * This file is part of the superbitrf project.
 *
 * Copyright (C) 2013 Freek van Tienen <freek.v.tienen@gmail.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libopencm3/cm3/common.h>
#include <libopencm3/stm32/gpio.h>
#include "../modules/timer.h"
#include "../modules/cyrf6936.h"
#include "../modules/led.h"

#include "dsm.h"
#include "convert.h"
#include "dsm_mitm.h"

//#if DEBUG
#include "../modules/cdcacm.h"
#include <stdio.h>
#include <string.h>
//#endif

/* The DSM mitm struct */
struct DsmMitm dsm_mitm;

static void dsm_mitm_create_data_packet(void);

/**
 * Start the receiver binding
 */
void dsm_mitm_start_bind(void) {
	// Stop the timer
	timer_dsm_stop();

	// Update the channel
#ifdef DSM_BIND_CHANNEL
	dsm_set_channel_raw(DSM_BIND_CHANNEL);
#else
	dsm_set_channel_raw(1);
#endif

	// Set the status to bind
	dsm_mitm.status = DSM_MITM_BIND;

	// Start receiving
	cyrf_start_recv();

	// Enable the timer
	timer_dsm_set(DSM_BIND_RECV_TIME);
}

/**
 * Start receiving
 */
void dsm_mitm_start(void) {
	// Stop the timer
	timer_dsm_stop();

	// Update the channel (goes to first because of ch_id = last) and set to SYNC
	if(IS_DSM2(dsm.protocol) || DSM_FORCE_DSM2)
		dsm_set_channel(0);
	else
		dsm_set_next_channel();
	dsm_mitm.status = DSM_MITM_SYNC_A;

	// Start receiving
	cyrf_start_recv();

	// Enable the timer
	if(IS_DSM2(dsm.protocol) || DSM_FORCE_DSM2)
		timer_dsm_set(DSM_SYNC_RECV_TIME);
	else
		timer_dsm_set(DSM_SYNC_FRECV_TIME); // Because we know for sure where DSMX starts we can wait the full bind FIXME
}

/**
 * When the timer send an IRQ
 */
void dsm_mitm_on_timer(void) {
	u8 rx_irq_status;

	// Abort the receive
	cyrf_set_mode(CYRF_MODE_SYNTH_RX, true);
	cyrf_write_register(CYRF_RX_ABORT, 0x00); //TODO: CYRF_RX_ABORT_EN

	// Check the receiver status
	switch (dsm_mitm.status) {
	case DSM_MITM_BIND:
#ifndef DSM_BIND_CHANNEL
		// Update the channel
		dsm_set_channel_raw((dsm.cur_channel + 2) % DSM_MAX_CHANNEL);
#endif

		// Start receiving
		cyrf_start_recv();

		// Set the new timeout
		timer_dsm_set(DSM_BIND_RECV_TIME);
		break;
	case DSM_MITM_SYNC_A:
	case DSM_MITM_SYNC_B:
		LED_ON(1);
		// When we are in DSM2 mode we need to scan all channels
		if(IS_DSM2(dsm.protocol) || DSM_FORCE_DSM2) {
			// Set the next channel
			dsm_set_channel((dsm.cur_channel + 1) % DSM_MAX_CHANNEL);
		} else {
			// Just set the next channel we know
			dsm_set_next_channel();
		}

		cyrf_start_recv();

		// Set the new timeout
		timer_dsm_set(DSM_SYNC_RECV_TIME);
		break;
	case DSM_MITM_RECV:
		if(dsm_mitm.missed_packets < 8) {
			// When we only miss one packet there is no problem
			dsm_mitm.missed_packets++;

#if DEBUG
	sprintf(cdc_msg, "Missed packet (0x%02X)\r\n", dsm.cur_channel);
	cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif

			// We still have to go to the next channel
			dsm_set_next_channel();
			cyrf_start_recv();

			// Start the timer
			timer_dsm_set(DSM_RECV_TIME);
		} else {
			// We are out of sync and start syncing again
			dsm_mitm.status = DSM_MITM_SYNC_A;

#if DEBUG
	sprintf(cdc_msg, "Missed too much packets (0x%02X)\r\n", dsm.cur_channel);
	cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif

			// Set the new timeout
			timer_dsm_set(DSM_SYNC_RECV_TIME);
		}
		break;
	default:
		break;
	}
}

/**
 * When the receive is completed (IRQ)
 * @param[in] error When the receive was with an error
 */
void dsm_mitm_on_receive(bool error) {
	int i;
	u16 sum;
	u8 rx_status = cyrf_get_rx_status();
	// Get the receive count
	u8 length = cyrf_read_register(CYRF_RX_COUNT);

	// Get the packet
	cyrf_recv_len(dsm.receive_packet, length);

	// Abort the receive
	cyrf_write_register(CYRF_XACT_CFG, CYRF_MODE_SYNTH_RX | CYRF_FRC_END);
	cyrf_write_register(CYRF_RX_ABORT, 0x00); //TODO: CYRF_RX_ABORT_EN

	// Check if length bigger then to
	if(length < 2)
		return;

#if DEBUG
	sprintf(cdc_msg, "DSM RECEIVE(0x%02X): [0x%02X 0x%02X] 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X %d\r\n",
			dsm.cur_channel, ~dsm.cyrf_mfg_id[2]&0xFF, ~dsm.cyrf_mfg_id[3]&0xFF,
			dsm.receive_packet[0], dsm.receive_packet[1], dsm.receive_packet[2], dsm.receive_packet[3], dsm.receive_packet[4], dsm.receive_packet[5],
			dsm.receive_packet[6], dsm.receive_packet[7], dsm.receive_packet[8], dsm.receive_packet[9], dsm.receive_packet[10], dsm.receive_packet[11],
			dsm.receive_packet[12], dsm.receive_packet[13], dsm.receive_packet[14], dsm.receive_packet[15], (dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF)+1));
	cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif

	// Check the receiver status
	switch (dsm_mitm.status) {
	case DSM_MITM_BIND:
		// Check if there is an error and the MFG id is exactly the same twice
		if (dsm.receive_packet[0] != dsm.receive_packet[4]
				|| dsm.receive_packet[1] != dsm.receive_packet[5]
				|| dsm.receive_packet[2] != dsm.receive_packet[6]
				|| dsm.receive_packet[3] != dsm.receive_packet[7]) {
			//FIXME
			// Set the new timeout
			timer_dsm_set(DSM_BIND_RECV_TIME);
			break;
		}

		// Calculate the first sum
		sum = 384 - 0x10;
		for(i = 0; i < 8; i++)
			sum += dsm.receive_packet[i];

		// Check the first sum
		if (dsm.receive_packet[8] != sum >> 8 || dsm.receive_packet[9] != (sum & 0xFF))
			break;

		// Calculate second sum
		for(i = 8; i < 14; i++)
			sum += dsm.receive_packet[i];

		// Check the second sum
		if (dsm.receive_packet[14] != sum >> 8 || dsm.receive_packet[15] != (sum & 0xFF))
			break;

		// Stop the timer
		timer_dsm_stop();

		// Update the mfg id, number of channels and protocol
		dsm.cyrf_mfg_id[0] = ~dsm.receive_packet[0];
		dsm.cyrf_mfg_id[1] = ~dsm.receive_packet[1];
		dsm.cyrf_mfg_id[2] = ~dsm.receive_packet[2];
		dsm.cyrf_mfg_id[3] = ~dsm.receive_packet[3];
		dsm_mitm.num_channels = dsm.receive_packet[11];
		dsm.protocol = dsm.receive_packet[12];
		dsm.resolution = (dsm.protocol & 0x10)>>4;

#if DEBUG
	sprintf(cdc_msg, "DSM RECEIVE(0x%02X): [0x%02X 0x%02X] 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\r\n",
			dsm.cur_channel, ~dsm.cyrf_mfg_id[2], ~dsm.cyrf_mfg_id[3],
			dsm.receive_packet[0], dsm.receive_packet[1], dsm.receive_packet[2], dsm.receive_packet[3], dsm.receive_packet[4], dsm.receive_packet[5],
			dsm.receive_packet[6], dsm.receive_packet[7], dsm.receive_packet[8], dsm.receive_packet[9], dsm.receive_packet[10], dsm.receive_packet[11],
			dsm.receive_packet[12], dsm.receive_packet[13], dsm.receive_packet[14], dsm.receive_packet[15]);
	cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif

		// Start receiver
		dsm_start();
		break;
	case DSM_MITM_SYNC_A:
		// If other error than bad CRC or MFG id doesn't match reject the packet
		if(error && !(rx_status & CYRF_BAD_CRC)) {
			break;
		}
		if((IS_DSM2(dsm.protocol) || DSM_FORCE_DSM2) &&
				(dsm.receive_packet[0] != (~dsm.cyrf_mfg_id[2]&0xFF) || (dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF)+1
						&& dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF)+2))) {
			cyrf_start_recv();
			break;
		}
		if((IS_DSMX(dsm.protocol) && !DSM_FORCE_DSM2) &&
				(dsm.receive_packet[0] != (dsm.cyrf_mfg_id[2]&0xFF) || (dsm.receive_packet[1] != ((dsm.cyrf_mfg_id[3]+1)&0xFF)
						&& dsm.receive_packet[1] != (dsm.cyrf_mfg_id[3]&0xFF)+2))) {
			cyrf_start_recv();
			break;
		}

#if DEBUG
	sprintf(cdc_msg, "SYNC A (0x%02X)\r\n", dsm.cur_channel);
	cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif
		// Invert the CRC when received bad CRC
		if (error && (rx_status & CYRF_BAD_CRC))
			dsm.crc_seed = ~dsm.crc_seed;

		// Stop the timer
		timer_dsm_stop();

		if(IS_DSM2(dsm.protocol) || DSM_FORCE_DSM2) {
			// We now got one channel and need to find the other
			dsm.channels[0] = dsm.cur_channel;
			dsm.channels[1] = dsm.cur_channel;
			dsm_mitm.status = DSM_MITM_SYNC_B;

			// Set the new channel and start receiving
			dsm_set_channel((dsm.cur_channel + 1) % DSM_MAX_CHANNEL);
			cyrf_start_recv();

			// Start the timer
			timer_dsm_set(DSM_SYNC_RECV_TIME);
		} else {
			// When it is DSMX we can stop because we know all the channels
			dsm_mitm.status = DSM_MITM_RECV;
			dsm_mitm.missed_packets = 0;

			// Set the next channel and start receiving
			dsm_set_next_channel();
			cyrf_start_recv();

			// Start the timer
			timer_dsm_set(DSM_RECV_TIME);
		}
		break;
	case DSM_MITM_SYNC_B:
		// If other error than bad CRC or MFG id doesn't match reject the packet
		if((error && !(rx_status & CYRF_BAD_CRC))
				|| dsm.receive_packet[0] != (~dsm.cyrf_mfg_id[2]&0xFF) ||
				(dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF)+1
						&& dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF)+2)) {
			cyrf_start_recv();
			break;
		}

		// Invert the CRC when received bad CRC
		if (error && (rx_status & CYRF_BAD_CRC))
			dsm.crc_seed = ~dsm.crc_seed;

		// Stop the timer
		timer_dsm_stop();

#if DEBUG
	sprintf(cdc_msg, "SYNC B (0x%02X)\r\n", dsm.cur_channel);
	cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif

		// We now got the other channel and can start receiving
		if (dsm.cur_channel != dsm.channels[0]) {
			dsm.channels[0] = dsm.cur_channel;
			dsm.ch_idx = 0;
		}
		else {
			dsm.channels[1] = dsm.cur_channel;
			dsm.ch_idx = 1;
		}

		if(dsm.channels[0] != dsm.channels[1])
			dsm_mitm.status = DSM_MITM_RECV;
		else {
			dsm_set_channel((dsm.cur_channel + 1) % DSM_MAX_CHANNEL);
			cyrf_start_recv();

			// Start the timer
			timer_dsm_set(DSM_SYNC_RECV_TIME);
			break;
		}

		// Set the next channel and start receiving
		dsm_set_next_channel();
		cyrf_start_recv();
		dsm_mitm.missed_packets = 0;

		// Start the timer
		timer_dsm_set(DSM_RECV_TIME);
		break;
	case DSM_MITM_RECV:
		LED_OFF(1);
		// If other error than bad CRC or MFG id doesn't match reject the packet
		if(error && !(rx_status & CYRF_BAD_CRC)) {
			break;
		}
		if((IS_DSM2(dsm.protocol) || DSM_FORCE_DSM2) &&
				(dsm.receive_packet[0] != (~dsm.cyrf_mfg_id[2]&0xFF) || (dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF)+1
						&& dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF)+2))) {
			cyrf_start_recv();
			break;
		}
		if((IS_DSMX(dsm.protocol) && !DSM_FORCE_DSM2) &&
				(dsm.receive_packet[0] != (dsm.cyrf_mfg_id[2]&0xFF) || (dsm.receive_packet[1] != (dsm.cyrf_mfg_id[3]&0xFF) &&
						dsm.receive_packet[1] != (dsm.cyrf_mfg_id[3]&0xFF)+1
						&& dsm.receive_packet[1] != (dsm.cyrf_mfg_id[3]&0xFF)+2))) {
			cyrf_start_recv();
			break;
		}

		// Invert the CRC when received bad CRC
		if (error && (rx_status & CYRF_BAD_CRC))
			dsm.crc_seed = ~dsm.crc_seed;

		// Stop the timer
		timer_dsm_stop();
		dsm_mitm.missed_packets = 0;

		// We got a data packet
		if(dsm.receive_packet[1] != (~dsm.cyrf_mfg_id[3]&0xFF) && dsm.receive_packet[1] != (dsm.cyrf_mfg_id[3]&0xFF)) {
			// Check for packet loss
			/*if(dsm.receive_packet[1] != dsm.cyrf_mfg_id[3] + 1 + dsm.packet_loss_bit)
				dsm.packet_loss = true;
			else
				dsm.packet_loss = false;*/

			// When we have packet loss we don't need to parse this again
			//if(!dsm.packet_loss)
#if !DEBUG
				convert_radio_to_cdcacm_insert(&dsm.receive_packet[2], length-2);
#endif

			//sprintf(cdc_msg, "YES!\r\n");
			//cdcacm_send(cdc_msg, strlen(cdc_msg));

			// Go to the next channel
			//dsm_set_next_channel();
			//cyrf_start_recv();

			// Start transmitting mode
			//cyrf_start_transmit();

			// Send data back or resend
			/*if(dsm.packet_loss)
				//cyrf_resend();
				cyrf_send_len(dsm.transmit_packet, dsm.transmit_packet_length);
			else {*/
				dsm_mitm_create_data_packet();
				//dsm.packet_loss_bit = !dsm.packet_loss_bit;
				cyrf_send_len(dsm.transmit_packet, dsm.transmit_packet_length);
			//}

			// Start the timer
			//timer_dsm_set(DSM_RECV_TIME);
		} else {
			// RC received
			static int16_t channels[14];
			convert_radio_to_channels(&dsm.receive_packet[2], dsm_mitm.num_channels, dsm.resolution, channels);
			cyrf_start_recv();
			//cyrf_send_len(dsm.transmit_packet, dsm.transmit_packet_length);

			// Start the timer
			timer_dsm_set(300);

#if DEBUG
	sprintf(cdc_msg, "Channels(%d):", dsm_mitm.num_channels);
	int i;
	for(i = 0; i < dsm_mitm.num_channels; i++)
		sprintf(cdc_msg, "%s\t%d", cdc_msg, channels[i]);
	sprintf(cdc_msg, "%s\r\n", cdc_msg);
	//cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif
		}
		break;
	default:
		break;
	}
}

/**
 * When the send is completed (IRQ)
 * @param[in] error When the send was with an error
 */
void dsm_mitm_on_send(bool error) {
#if DEBUG
	sprintf(cdc_msg, "DSM SEND(0x%02X): [0x%02X 0x%02X] 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\r\n",
			dsm.cur_channel, error, dsm.transmit_packet_length,
			dsm.transmit_packet[0], dsm.transmit_packet[1], dsm.transmit_packet[2], dsm.transmit_packet[3], dsm.transmit_packet[4], dsm.transmit_packet[5],
			dsm.transmit_packet[6], dsm.transmit_packet[7], dsm.transmit_packet[8], dsm.transmit_packet[9], dsm.transmit_packet[10], dsm.transmit_packet[11],
			dsm.transmit_packet[12], dsm.transmit_packet[13], dsm.transmit_packet[14], dsm.transmit_packet[15]);
	cdcacm_send(cdc_msg, strlen(cdc_msg));
#endif

	// Set the next channel
	dsm_set_next_channel();
	cyrf_start_recv();

	// Start the timer
	timer_dsm_set(DSM_RECV_TIME);
}

/**
 * Create the receiver data packet
 */
static void dsm_mitm_create_data_packet(void) {
	u8 i;
	// Set the first two bytes to the cyrf_mfg_id
	if(IS_DSM2(dsm.protocol) || DSM_FORCE_DSM2) {
		dsm.transmit_packet[0] = ~dsm.cyrf_mfg_id[2];
		dsm.transmit_packet[1] = ~dsm.cyrf_mfg_id[3] + 1;
	} else {
		dsm.transmit_packet[0] = dsm.cyrf_mfg_id[2];
		dsm.transmit_packet[1] = dsm.cyrf_mfg_id[3] + 1;
	}

	// Fill as much bytes with data
	dsm.transmit_packet_length = convert_extract(&cdcacm_to_radio, dsm.transmit_packet+2, 14) + 2;
	// Clean the rest of the packet
	for(i = dsm.transmit_packet_length; i < 16; i++)
		dsm.transmit_packet[i] = 0x00;
}
