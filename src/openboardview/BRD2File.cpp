#include "BRD2File.h"

#include <assert.h>
#include <ctype.h>
#include <iostream>
#include <stdint.h>
#include <string.h>
#include <unordered_map>

bool BRD2File::verifyFormat(const char *buf, size_t buffer_size) {
	std::string sbuf(buf, buffer_size);
	if ((sbuf.find("BRDOUT:") != std::string::npos) && (sbuf.find("NETS:") != std::string::npos)) return true;
	return false;
}

BRD2File::BRD2File(const char *buf, size_t buffer_size) {
	std::unordered_map<int, char *> nets; // Map between net id and net name
	unsigned int num_nets = 0;
	BRDPoint max{0, 0}; // Top-right board boundary

	ENSURE(buffer_size > 4);
	size_t file_buf_size = 3 * (1 + buffer_size);
	file_buf             = (char *)calloc(1, file_buf_size);
	ENSURE(file_buf != nullptr);
	memcpy(file_buf, buf, buffer_size);
	file_buf[buffer_size] = 0;
	// This is for fixing degenerate utf8
	char *arena     = &file_buf[buffer_size + 1];
	char *arena_end = file_buf + file_buf_size - 1;
	*arena_end      = 0;

	int current_block = 0;

	char **lines = stringfile(file_buf);
	ENSURE(lines)

	while (*lines) {
		char *line = *lines;
		++lines;

		while (isspace((uint8_t)*line)) line++;
		if (!line[0]) continue;

		char *p = line;
		char *s;

		if (strstr(line, "BRDOUT:") == line) {
			current_block = 1;
			p += 7; // Skip "BRDOUT:"
			num_format = READ_UINT();
			max.x      = READ_INT();
			max.y      = READ_INT();
			continue;
		}
		if (strstr(line, "NETS:") == line) {
			current_block = 2;
			p += 5; // Skip "NETS:"
			num_nets = READ_UINT();
			continue;
		}
		if (strstr(line, "PARTS:") == line) {
			current_block = 3;
			p += 6; // Skip "PARTS:"
			num_parts = READ_UINT();
			continue;
		}
		if (strstr(line, "PINS:") == line) {
			current_block = 4;
			p += 5; // Skip "PINS:"
			num_pins = READ_UINT();
			continue;
		}
		if (strstr(line, "NAILS:") == line) {
			current_block = 5;
			p += 6; // Skip "NAILS:"
			num_nails = READ_UINT();
			continue;
		}

		switch (current_block) {
			case 1: { // Format
				ENSURE(format.size() < num_format);
				BRDPoint point;
				point.x = READ_INT();
				point.y = READ_INT();
				ENSURE(point.x <= max.x);
				ENSURE(point.y <= max.y);
				format.push_back(point);
			} break;

			case 2: { // Nets
				ENSURE(nets.size() < num_nets);
				int id   = READ_UINT();
				nets[id] = READ_STR();
			} break;

			case 3: { // PARTS
				ENSURE(parts.size() < num_parts);
				BRDPart part;

				part.name        = READ_STR();
				part.p1.x        = READ_INT();
				part.p1.y        = READ_INT();
				part.p2.x        = READ_INT();
				part.p2.y        = READ_INT();
				part.end_of_pins = READ_UINT(); // Warning: not end but beginning in this format
				int side         = READ_UINT();
				if (side == 1)
					part.type = 10; // SMD part on top
				else if (side == 2)
					part.type = 5; // SMD part on bottom

				parts.push_back(part);
			} break;

			case 4: { // PINS
				ENSURE(pins.size() < num_pins);
				BRDPin pin;

				pin.pos.x = READ_INT();
				pin.pos.y = READ_INT();
				int netid = READ_UINT();
				pin.side  = READ_UINT();

				try {
					pin.net = nets.at(netid);
				} catch (const std::out_of_range &e) {
					pin.net = "";
				}

				pin.probe = 1;
				pin.part  = 0;
				pins.push_back(pin);
			} break;

			case 5: { // NAILS
				ENSURE(nails.size() < num_nails);
				BRDNail nail;

				nail.probe = READ_UINT();
				nail.pos.x = READ_INT();
				nail.pos.y = READ_INT();
				int netid  = READ_UINT();
				nail.net   = nets.at(netid);
				nail.side  = READ_UINT();
				nails.push_back(nail);

			} break;
			default: continue;
		}
	}

	ENSURE(num_format == format.size());
	ENSURE(num_nets == nets.size());
	ENSURE(num_parts == parts.size());
	ENSURE(num_pins == pins.size());
	ENSURE(num_nails == nails.size());

	/*
	 * Postprocess the data.  Specifically we need to allocate
	 * pins to parts.  So with this it's easiest to just iterate
	 * through the parts and pick up the pins required
	 */
	{
		int pei;     // pin end index (part[i+1].pin# -1
		int cpi = 0; // current pin index
		for (decltype(parts)::size_type i = 0; i < parts.size(); i++) {
			bool isDIP = true;

			if (parts[i].type == 5) { // Part on bottom
				parts[i].p1.y = max.y - parts[i].p1.y;
				parts[i].p2.y = max.y - parts[i].p2.y;
			}

			if (i == parts.size() - 1) {
				pei = pins.size();
			} else {
				pei = parts[i + 1].end_of_pins; // Again, not end of pins but beginning
			}

			while (cpi < pei) {
				pins[cpi].part                           = i + 1;
				if (pins[cpi].side != 1) pins[cpi].pos.y = max.y - pins[cpi].pos.y;
				if ((pins[cpi].side == 1 && parts[i].type == 10) ||
				    (pins[cpi].side == 2 && parts[i].type == 5)) // Pins on the same side as the part
					isDIP = false;
				cpi++;
			}

			if (isDIP) parts[i].type /= 5; // All pins are through hole so part is DIP
		}
	}

	for (auto i = 1; i <= 2; i++) { // Add dummy parts for probe points on both sides
		BRDPart part;
		part.name        = "...";
		part.type        = 5 * i; // First part is bottom, last is top.
		part.end_of_pins = 0;     // Unused
		parts.push_back(part);
	}

	for (auto &nail : nails) {
		BRDPin pin;
		pin.pos = nail.pos;
		if (nail.side == 1) {
			pin.part = parts.size();
		} else {
			pin.pos.y = max.y - pin.pos.y;
			pin.part  = parts.size() - 1;
		}
		pin.probe = nail.probe;
		pin.side  = nail.side;
		pin.net   = nail.net;
		pins.push_back(pin);
	}

	valid = current_block != 0;
}
