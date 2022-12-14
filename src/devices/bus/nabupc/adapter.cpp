// license:BSD-3-Clause
// copyright-holders:Brian Johnson
/*******************************************************************
 *
 * NABU PC - Network Adapter Settop Box
 *
 *******************************************************************/

#include "emu.h"
#include "adapter.h"

#include "emuopts.h"

#define VERBOSE 0
#include "logmacro.h"


//**************************************************************************
//  NABU PC NETWORK ADAPTER DEVICE
//**************************************************************************

DEFINE_DEVICE_TYPE(NABUPC_NETWORK_ADAPTER, bus::nabupc::network_adapter, "nabu_net_adapter", "NABU Network Adapter")

namespace bus::nabupc {

//**************************************************************************
//  SEGMENT FILE LOADING
//**************************************************************************

// Load segment file from disk
std::error_condition network_adapter::segment_file::load(std::string_view local_path, uint32_t segment_id)
{
	segment_id &= 0xFFFFFF;

	osd_file::ptr fd;
	pak current;
	uint64_t segment_size;
	uint64_t offset = 0;
	uint32_t actual;
	uint16_t crc;
	uint8_t npak = 0;
	std::error_condition err;
	std::string filename = util::string_format("%s/nabu_network/%06d.nabu", osd_subst_env(local_path), segment_id);
	pak_list.clear();
	err = osd_file::open(filename, OPEN_FLAG_READ, fd, segment_size);
	if (err)
		return err;

	memset(current.data, 0, 991);
	current.segment_id[0] = (segment_id & 0xFF0000) >> 16;
	current.segment_id[1] = (segment_id & 0x00FF00) >> 8;
	current.segment_id[2] = (segment_id & 0xF000FF);
	current.owner         = 0x01;
	current.tier[0]       = 0x7f;
	current.tier[1]       = 0xff;
	current.tier[2]       = 0xff;
	current.tier[3]       = 0xff;
	current.mbytes[0]     = 0x7f;
	current.mbytes[1]     = 0x80;
	err = fd->read(current.data, offset, 991, actual);
	do {
		crc = 0xffff;
		if (err) {
			return err;
		}
		if (actual > 0) {
			current.packet_number = npak;
			current.pak_number[0] = npak;
			current.pak_number[1] = 0;
			current.type = 0x20;
			if (offset == 0)
				current.type |= 0x81;
			if (actual < 991)
				current.type |= 0x10;
			current.offset[0] = ((offset) >> 8) & 0xFF;
			current.offset[1] = offset & 0xFF;
			for (int i = 0; i < 1007; ++i) {
				crc = update_crc(crc, ((char *)&current)[i]);
			}
			crc ^= 0xffff;
			current.crc[0] = (crc >> 8) & 0xFF;
			current.crc[1] = crc & 0xFF;
			pak_list.push_back(current);
			offset = (++npak * 991);
			memset(current.data, 0, 991);
			err = fd->read(current.data, offset, 991, actual);
		}
	} while(actual > 0);

	return err;
}

const network_adapter::segment_file::pak& network_adapter::segment_file::operator[](const int index) const
{
	assert(index >= 0 && index < size());

	return pak_list[index];
}

// crc16 calculation
uint16_t network_adapter::segment_file::update_crc(uint16_t crc, uint8_t data)
{
	uint8_t bc;

	bc = (crc >> 8) ^ data;

	crc <<= 8;
	crc ^= crc_table[bc];

	return crc;
}

//**************************************************************************
//  INPUT PORTS
//**************************************************************************

// CONFIG
static INPUT_PORTS_START( nabu_network_adapter )
	PORT_START("CONFIG")
	PORT_CONFNAME(0x01, 0x00, "Prompt for channel?")
	PORT_CONFSETTING(0x01, "Yes")
	PORT_CONFSETTING(0x00, "No")
INPUT_PORTS_END

//**************************************************************************
//  DEVICE INITIALIZATION
//**************************************************************************

network_adapter::network_adapter(machine_config const &mconfig, char const *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, NABUPC_NETWORK_ADAPTER, tag, owner, clock)
	, device_buffered_serial_interface(mconfig, *this)
	, device_rs232_port_interface(mconfig, *this)
	, m_config(*this, "CONFIG")
	, m_channel(0)
	, m_packet(0)
	, m_segment(0)
	, m_state(state::START)
	, m_substate(0)
	, m_segment_timer(nullptr)
{
}

void network_adapter::device_start()
{
	m_segment_timer = timer_alloc(FUNC(network_adapter::segment_tick), this);

	machine().save().register_postload(save_prepost_delegate(FUNC(network_adapter::postload), this));

	save_item(NAME(m_state));
	save_item(NAME(m_substate));
	save_item(NAME(m_channel));
	save_item(NAME(m_packet));
	save_item(NAME(m_segment));
}

void network_adapter::device_reset()
{
	m_state = state::START;
	m_substate = 0;
	// initialise state
	clear_fifo();

	// configure device_buffered_serial_interface
	set_data_frame(START_BIT_COUNT, DATA_BIT_COUNT, PARITY, STOP_BITS);
	set_rate(BAUD);
	receive_register_reset();
	transmit_register_reset();
}

void network_adapter::postload()
{
	m_cache.load(machine().options().share_directory(), m_segment);
}

//**************************************************************************
//  DEVICE CONFIGURATION
//**************************************************************************

ioport_constructor network_adapter::device_input_ports() const
{
	return INPUT_PORTS_NAME( nabu_network_adapter );
}

//**************************************************************************
//  SERIAL PROTOCOL
//**************************************************************************

WRITE_LINE_MEMBER(network_adapter::input_txd)
{
	device_buffered_serial_interface::rx_w(state);
}

void network_adapter::tra_callback()
{
	output_rxd(transmit_register_get_data_bit());
}

void network_adapter::received_byte(uint8_t byte)
{
	LOG("Received Byte 0x%02X\n", byte);
	switch (m_state) {
	case state::START:
		connect(byte, bool(m_config->read() & 1));
		break;
	case state::IDLE:
		idle(byte);
		break;
	case state::CHANNEL_REQUEST:
		channel_request(byte);
		break;
	case state::SEGMENT_REQUEST:
		segment_request(byte);
		break;
	case state::HEX81_REQUEST:
		hex81_request(byte);
		break;
	case state::SEND_SEGMENT:
		send_segment(byte);
		break;
	}
}

//**************************************************************************
//  STATE MACHINE
//**************************************************************************

void network_adapter::connect(uint8_t byte, bool channel_request = true)
{
	if (byte == 0x83 && m_substate == 0) {
		transmit_byte(0x10);
		transmit_byte(0x06);
		transmit_byte(0xE4);
	} else if (byte == 0x82 && m_substate == 1) {
		transmit_byte(0x10);
		transmit_byte(0x06);
	} else if (byte == 0x01 && m_substate == 2) {
		transmit_byte(channel_request ? 0x9F : 0x1F);
		transmit_byte(0x10);
		transmit_byte(0xE1);
		m_state = state::IDLE;
	} else {
		LOG("Unexpected byte: 0x%02X (%d), restarting Adapter.\n", byte, m_substate);
		m_state = state::START;
		m_substate = 0;
		return;
	}
	++m_substate;
}

void network_adapter::idle(uint8_t byte)
{
	m_substate = 0;
	switch (byte) {
	case 0x85:
		transmit_byte(0x10);
		transmit_byte(0x06);
		m_state = state::CHANNEL_REQUEST;
		break;
	case 0x84:
		transmit_byte(0x10);
		transmit_byte(0x06);
		m_state = state::SEGMENT_REQUEST;
		break;
	case 0x83:
		transmit_byte(0x10);
		transmit_byte(0x06);
		break;
	case 0x82:
		transmit_byte(0x10);
		transmit_byte(0x06);
		break;
	case 0x81:
		transmit_byte(0x10);
		transmit_byte(0x06);
		m_state = state::HEX81_REQUEST;
		break;
	case 0x01:
		transmit_byte(0x10);
		transmit_byte(0x06);
		break;
	}

}

void network_adapter::hex81_request(uint8_t byte)
{
	if (m_substate == 1) {
		transmit_byte(0xE4);
		m_state = state::IDLE;
	}
	++m_substate;

}

void network_adapter::channel_request(uint8_t byte)
{
	if (m_substate == 0) {
		m_channel = (m_channel & 0xFF00) | (byte);
	} else if (m_substate == 1) {
		m_channel = (m_channel & 0xFF) | (byte << 8);
		transmit_byte(0xE4);
		LOG("Channel: 0x%04X\n", m_channel);
		m_state = state::IDLE;
	}
	++m_substate;
}

void network_adapter::segment_request(uint8_t byte)
{
	if (m_substate == 0) {
		m_packet = byte;
	} else if (m_substate == 1) {
		m_segment = (m_segment & 0xFFFF00) | (byte);
	} else if (m_substate == 2) {
		m_segment = (m_segment & 0xFF00FF) | (byte << 8);
	} else if (m_substate == 3) {
		m_segment = (m_segment & 0xFFFF) | (byte << 16);
		transmit_byte(0xE4);
		transmit_byte(0x91);
		m_state = state::SEND_SEGMENT;
		m_substate = 0;
		LOG("Segment: 0x%06X, Packet: 0x%02X\n", m_segment, m_packet);
		return;
	}
	++m_substate;
}

void network_adapter::send_segment(uint8_t byte)
{
	if (m_substate == 0) {
		if (byte != 0x10) {
			LOG("Expecting byte 0x10 got %02X, restarting.\n", byte);
			m_state = state::START;
			m_substate = 0;
			return;
		}
	} else if (m_substate == 1) {
		if (byte != 0x06) {
			LOG("Expecting byte 0x06 got %02X, restarting.\n", byte);
			m_state = state::START;
			m_substate = 0;
			return;
		}
		m_pak_offset = 0;
		if (!m_cache.load(machine().options().share_directory(), m_segment)) {
			m_segment_timer->adjust(attotime::zero, 0, attotime::from_hz(1'000));
			LOG("Segment sending, returning to idle state\n");
		} else {
			LOG("Failed to find segment: %06d, restarting\n", m_segment);
			transmit_byte(0x10);
			transmit_byte(0x06);
			transmit_byte(0xE4);
		}
		m_state = state::IDLE;
	}

	++m_substate;
}

TIMER_CALLBACK_MEMBER(network_adapter::segment_tick)
{
		char * data = (char*)&m_cache[m_packet];
		if (data[m_pak_offset] == 0x10) {
			transmit_byte(data[m_pak_offset]);
		}
		transmit_byte(data[m_pak_offset++]);
		if (m_pak_offset >= sizeof(segment_file::pak)) {
			transmit_byte(0x10);
			transmit_byte(0xe1);
			m_segment_timer->reset();
		}
}

} // bus::nabupc
