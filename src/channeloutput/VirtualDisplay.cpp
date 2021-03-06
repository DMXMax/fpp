/*
 *   VirtualDisplay Channel Output for Falcon Player (FPP)
 *
 *   Copyright (C) 2013-2018 the Falcon Player Developers
 *      Initial development by:
 *      - David Pitts (dpitts)
 *      - Tony Mace (MyKroFt)
 *      - Mathew Mrosko (Materdaddy)
 *      - Chris Pinkham (CaptainMurdoch)
 *      For additional credits and developers, see credits.php.
 *
 *   The Falcon Player (FPP) is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "log.h"
#include "VirtualDisplay.h"
#include "Sequence.h"
#include "settings.h"

/////////////////////////////////////////////////////////////////////////////
// To disable interpolated scaling on the GPU, add this to /boot/config.txt:
// scaling_kernel=8

/*
 *
 */
VirtualDisplayOutput::VirtualDisplayOutput(unsigned int startChannel,
	unsigned int channelCount)
  : ThreadedChannelOutputBase(startChannel, channelCount),
	m_backgroundFilename("virtualdisplaybackground.jpg"),
	m_backgroundBrightness(0.5),
	m_width(1280),
	m_height(1024),
	m_bytesPerPixel(3),
	m_bpp(24),
	m_scale(1.0),
	m_previewWidth(800),
	m_previewHeight(600),
	m_colorOrder("RGB"),
	m_virtualDisplay(NULL),
	m_pixelSize(2),
	m_rgb565map(nullptr)
{
	LogDebug(VB_CHANNELOUT, "VirtualDisplayOutput::VirtualDisplayOutput(%u, %u)\n",
		startChannel, channelCount);

	m_maxChannels = FPPD_MAX_CHANNELS;
	m_useDoubleBuffer = 1;
}

/*
 *
 */
VirtualDisplayOutput::~VirtualDisplayOutput()
{
	LogDebug(VB_CHANNELOUT, "VirtualDisplayOutput::~VirtualDisplayOutput()\n");

	if (m_rgb565map)
	{
		for (int r = 0; r < 32; r++)
		{
			for (int g = 0; g < 64; g++)
			{
				delete[] m_rgb565map[r][g];
			}

			delete[] m_rgb565map[r];
		}

		delete[] m_rgb565map;
	}
}

/*
 *
 */
int VirtualDisplayOutput::Init(Json::Value config)
{
	LogDebug(VB_CHANNELOUT, "VirtualDisplayOutput::Init()\n");

	m_width = config["width"].asInt();
	if (!m_width)
		m_width = 1280;

	m_height = config["height"].asInt();
	if (!m_height)
		m_height = 1024;

	m_pixelSize = config["pixelSize"].asInt();
	if (!m_pixelSize)
		m_pixelSize = 2;

	if (config.isMember("colorOrder"))
		m_colorOrder = config["colorOrder"].asString();

	if (config.isMember("backgroundFilename"))
		m_backgroundFilename = config["backgroundFilename"].asString();

	if (config.isMember("backgroundBrightness"))
		m_backgroundBrightness = 1.0 * config["backgroundBrightness"].asInt() / 100;

	return ThreadedChannelOutputBase::Init(config);
}

/*
 *
 */
int VirtualDisplayOutput::InitializePixelMap(void)
{
	std::string virtualDisplayMapFilename(getMediaDirectory());
	virtualDisplayMapFilename += "/config/virtualdisplaymap";

	if (!FileExists(virtualDisplayMapFilename.c_str()))
	{
		LogErr(VB_CHANNELOUT, "Error: %s does not exist\n",
			virtualDisplayMapFilename.c_str());
		return 0;
	}

	FILE *file = fopen(virtualDisplayMapFilename.c_str(), "r");

	if (file == NULL)
	{
		LogErr(VB_CHANNELOUT, "Error: unable to open %s: %s\n",
			virtualDisplayMapFilename.c_str(), strerror(errno));
		return 0;
	}

	char * line = NULL;
	size_t len = 0;
	ssize_t read;

	std::string pixel;
	std::vector<std::string> parts;
	int x = 0;
	int y = 0;
	int ch = 0;
	int s = 0;
	int r = 0;
	int g = 0;
	int b = 0;
	int rowOffset = 0;
	int colOffset = 0;
	int first = 1;
	int i = 0;
	int found = 0;
	VirtualPixelColor vpc = kVPC_RGB;

	while ((read = getline(&line, &len, file)) != -1)
	{
		if (( ! line ) || ( ! read ) || ( read == 1 ))
			continue;

		if (line[0] == '#')
			continue;

		// Strip the line feed
		line[strlen(line) - 1] = '\0';

		pixel = line;
		parts = split(pixel, ',');

		if (first)
		{
			if (parts.size() == 2)
			{
				m_previewWidth  = atoi(parts[0].c_str());
				m_previewHeight = atoi(parts[1].c_str());

				if ((m_width == -1) || (m_height == -1))
				{
					m_width = m_previewWidth;
					m_height = m_previewHeight;

					m_virtualDisplay = (unsigned char *)malloc(m_width * m_height * m_bytesPerPixel);
					if (!m_virtualDisplay)
					{
						LogErr(VB_CHANNELOUT, "Unable to malloc buffer\n");
						return 0;
					}
				}

				if ((1.0 * m_width / m_previewWidth) > (1.0 * m_height / m_previewHeight))
				{
					// Virtual Display is wider than the preview aspect, so background
					// image height should equal virtual display height
					m_scale = 1.0 * m_height / m_previewHeight;
					rowOffset = 0;
					colOffset = ((m_width - (m_previewWidth * m_scale)) / 2);
				}
				else
				{
					// Virtual Display is taller than the preview aspect, so background
					// image width should equal virtual display width
					m_scale = 1.0 * m_width / m_previewWidth;
					colOffset = 0;
					rowOffset = (m_height - (m_previewHeight * m_scale));
				}
			}

			first = 0;
			continue;
		}

		if (parts.size() >= 3)
		{
			x  = atoi(parts[0].c_str());
			y  = atoi(parts[1].c_str());
			ch = atoi(parts[2].c_str());

			s = ((m_height - (int)(y * m_scale) - 1) * m_width
					+ (int)(x * m_scale + colOffset)) * m_bytesPerPixel;

			if (m_colorOrder == "RGB")
			{
				r = s + 0;
				g = s + 1;
				b = s + 2;
			}
			else if (m_colorOrder == "RBG")
			{
				r = s + 0;
				g = s + 2;
				b = s + 1;
			}
			else if (m_colorOrder == "GRB")
			{
				r = s + 1;
				g = s + 0;
				b = s + 2;
			}
			else if (m_colorOrder == "GBR")
			{
				r = s + 2;
				g = s + 0;
				b = s + 1;
			}
			else if (m_colorOrder == "BRG")
			{
				r = s + 1;
				g = s + 2;
				b = s + 0;
			}
			else if (m_colorOrder == "BGR")
			{
				r = s + 2;
				g = s + 1;
				b = s + 0;
			}

			if (parts[4] == "RGB")
				vpc = kVPC_RGB;
			else if (parts[4] == "RBG")
				vpc = kVPC_RBG;
			else if (parts[4] == "GRB")
				vpc = kVPC_GRB;
			else if (parts[4] == "GBR")
				vpc = kVPC_GBR;
			else if (parts[4] == "BRG")
				vpc = kVPC_BRG;
			else if (parts[4] == "BGR")
				vpc = kVPC_BGR;
			else if (parts[4] == "RGBW")
				vpc = kVPC_RGBW;
			else if (parts[4] == "Red")
				vpc = kVPC_Red;
			else if (parts[4] == "Green")
				vpc = kVPC_Green;
			else if (parts[4] == "Blue")
				vpc = kVPC_Blue;
			else if (parts[4] == "White")
				vpc = kVPC_White;

			found = 0;
			for (i = 0; i < m_pixels.size() && !found; i++)
			{
				if ((m_pixels[i].x == x) && (m_pixels[i].y == y))
					found = 1;
			}

			if (!found)
				m_pixels.push_back({ x, y, ch, r, g, b, atoi(parts[3].c_str()), vpc });
		}
	}

	LoadBackgroundImage();

	return 1;
}

/*
 *
 */
int VirtualDisplayOutput::ScaleBackgroundImage(std::string &bgFile, std::string &rgbFile)
{
	std::string command;

	command = "convert -scale " + std::to_string(m_width) + "x"
		+ std::to_string(m_height) + " " + bgFile + " " + rgbFile;

	LogDebug(VB_CHANNELOUT, "Generating scaled RGB background image: %s\n", command.c_str());
	system(command.c_str());

	return 1;
}

/*
 *
 */
void VirtualDisplayOutput::LoadBackgroundImage(void)
{
	std::string bgFile = "/home/fpp/media/upload/";
	bgFile += m_backgroundFilename;

	std::string rgbFile = bgFile + ".rgb";

	if (!FileExists(bgFile))
	{
		LogErr(VB_CHANNELOUT, "Background image does not exist: %s\n",
			bgFile.c_str());
		return;
	}

	if ((!FileExists(rgbFile)) && (!ScaleBackgroundImage(bgFile, rgbFile)))
		return;

	struct stat ostat;
	struct stat sstat;

	stat(bgFile.c_str(), &ostat);
	stat(rgbFile.c_str(), &sstat);

	// Check if original is newer than scaled version
	if ((ostat.st_mtim.tv_sec > sstat.st_mtim.tv_sec) && (!ScaleBackgroundImage(bgFile, rgbFile)))
		return;

	FILE *fd = fopen(rgbFile.c_str(), "rb");
	if (fd)
	{
		unsigned char buf[3072];
		int bytesRead = -1;
		int offset = 0;
		unsigned long RGBx;
		int imgWidth = 0;
		int imgHeight = 0;

		if ((1.0 * m_width / m_previewWidth) < (1.0 * m_height / m_previewHeight))
		{
			// Virtual Display is taller than the preview aspect, so background
			// image width should equal virtual display width
			imgWidth = m_width;
			imgHeight = sstat.st_size / m_width / 3;
			offset = (m_height - imgHeight) * m_width * m_bytesPerPixel;

			while (bytesRead != 0)
			{
				bytesRead = fread(buf, 1, 3072, fd);

				for (int i = 0; i < bytesRead;)
				{
					if ((m_bpp == 24) || (m_bpp == 32))
					{
						m_virtualDisplay[offset++] = buf[i++] * m_backgroundBrightness;
						m_virtualDisplay[offset++] = buf[i++] * m_backgroundBrightness;
						m_virtualDisplay[offset++] = buf[i++] * m_backgroundBrightness;

						if (m_bpp == 32)
							offset++;
					}
					else // 16bpp RGB565
					{
						RGBx = *(unsigned long *)(buf + i);
						*(uint16_t*)(m_virtualDisplay + offset) =
							(((int)((RGBx & 0x000000FF) * m_backgroundBrightness) <<  8) & 0b1111100000000000) |
							(((int)((RGBx & 0x0000FF00) * m_backgroundBrightness) >>  5) & 0b0000011111100000) |
							(((int)((RGBx & 0x00FF0000) * m_backgroundBrightness) >> 19)); // & 0b0000000000011111);

						i += 3;
						offset += 2;
					}
				}
			}
		}
		else
		{
			// Virtual Display is wider than the preview aspect, so background
			// image height should equal virtual display height
			imgHeight = m_height;
			imgWidth = sstat.st_size / m_height / 3;

			int colOffset = (m_width - imgWidth) / 2;
			int r = 0;
			int c = 0;

			while (bytesRead != 0)
			{
				bytesRead = fread(buf, 1, 3072, fd);

				for (int i = 0; i < bytesRead;)
				{
					if (c == 0)
					{
						offset = ((r * m_width) + colOffset) * m_bytesPerPixel;
					}

					if ((m_bpp == 24) || (m_bpp == 32))
					{
						m_virtualDisplay[offset++] = buf[i++] * m_backgroundBrightness;
						m_virtualDisplay[offset++] = buf[i++] * m_backgroundBrightness;
						m_virtualDisplay[offset++] = buf[i++] * m_backgroundBrightness;

						if (m_bpp == 32)
							offset++;
					}
					else
					{
						RGBx = *(unsigned long *)(buf + i);
						*(uint16_t*)(m_virtualDisplay + offset) =
							(((int)((RGBx & 0x000000FF) * m_backgroundBrightness) <<  8) & 0b1111100000000000) |
							(((int)((RGBx & 0x0000FF00) * m_backgroundBrightness) >>  5) & 0b0000011111100000) |
							(((int)((RGBx & 0x00FF0000) * m_backgroundBrightness) >> 19)); // & 0b0000000000011111);

						i += 3;
						offset += 2;
					}

					c++;

					if (c >= imgWidth)
					{
						c = 0;
						r++;
					}
				}
			}
		}

		fclose(fd);
	}
}


/*
 *
 */
void VirtualDisplayOutput::DrawPixel(int rOffset, int gOffset, int bOffset,
	unsigned char r, unsigned char g, unsigned char b)
{
	if (m_bpp == 16)
	{
		if ((rOffset < gOffset) && (gOffset < bOffset))
		{
			*((uint16_t*)(m_virtualDisplay + rOffset)) = m_rgb565map[r >> 3][g >> 2][b >> 3];
		}
		else if ((rOffset < gOffset) && (bOffset < gOffset))
		{
			*((uint16_t*)(m_virtualDisplay + rOffset)) = m_rgb565map[r >> 3][b >> 2][g >> 3];
		}
		else if ((gOffset < rOffset) && (rOffset < bOffset))
		{
			*((uint16_t*)(m_virtualDisplay + gOffset)) = m_rgb565map[g >> 3][r >> 2][b >> 3];
		}
		else if ((gOffset < rOffset) && (bOffset < rOffset))
		{
			*((uint16_t*)(m_virtualDisplay + gOffset)) = m_rgb565map[g >> 3][b >> 2][r >> 3];
		}
		else if ((bOffset < gOffset) && (rOffset < gOffset))
		{
			*((uint16_t*)(m_virtualDisplay + bOffset)) = m_rgb565map[b >> 3][r >> 2][g >> 3];
		}
		else if ((bOffset < gOffset) && (gOffset < rOffset))
		{
			*((uint16_t*)(m_virtualDisplay + bOffset)) = m_rgb565map[b >> 3][g >> 2][r >> 3];
		}
	}
	else
	{
		m_virtualDisplay[rOffset] = r;
		m_virtualDisplay[gOffset] = g;
		m_virtualDisplay[bOffset] = b;
	}
}

void VirtualDisplayOutput::GetRequiredChannelRange(int &min, int & max) {
    min = FPPD_MAX_CHANNELS;
    max = 0;
    for (auto &pixel : m_pixels) {
        min = std::min(min, pixel.ch);
        max = std::max(max, pixel.ch + (pixel.cpp == 4 ? 3 : 2));
    }
}


/*
 *
 */
void VirtualDisplayOutput::DrawPixels(unsigned char *channelData)
{
	unsigned char r = 0;
	unsigned char g = 0;
	unsigned char b = 0;
	int stride = m_width * m_bytesPerPixel;
	VirtualDisplayPixel pixel;

	for (int i = 0; i < m_pixels.size(); i++)
	{
		pixel = m_pixels[i];

		GetPixelRGB(pixel, channelData, r, g, b);

		if (m_pixelSize == 2)
		{
			DrawPixel(pixel.r, pixel.g, pixel.b, r, g, b);

			r /= 2;
			g /= 2;
			b /= 2;

			DrawPixel(pixel.r + m_bytesPerPixel,
					  pixel.g + m_bytesPerPixel,
					  pixel.b + m_bytesPerPixel,
					  r, g, b);
			DrawPixel(pixel.r - m_bytesPerPixel,
					  pixel.g - m_bytesPerPixel,
					  pixel.b - m_bytesPerPixel,
					  r, g, b);
			DrawPixel(pixel.r + stride,
					  pixel.g + stride,
					  pixel.b + stride,
					  r, g, b);
			DrawPixel(pixel.r - stride,
					  pixel.g - stride,
					  pixel.b - stride,
					  r, g, b);
		}
		else
		{
			DrawPixel(pixel.r, pixel.g, pixel.b, r, g, b);
		}
	}
}


/*
 *
 */
void VirtualDisplayOutput::DumpConfig(void)
{
	LogDebug(VB_CHANNELOUT, "VirtualDisplayOutput::DumpConfig()\n");
	LogDebug(VB_CHANNELOUT, "    width         : %d\n", m_width);
	LogDebug(VB_CHANNELOUT, "    height        : %d\n", m_height);
	LogDebug(VB_CHANNELOUT, "    scale         : %0.3f\n", m_scale);
	LogDebug(VB_CHANNELOUT, "    preview width : %d\n", m_previewWidth);
	LogDebug(VB_CHANNELOUT, "    preview height: %d\n", m_previewHeight);
	LogDebug(VB_CHANNELOUT, "    color Order   : %s\n", m_colorOrder.c_str());
	LogDebug(VB_CHANNELOUT, "    pixel count   : %d\n", m_pixels.size());
	LogDebug(VB_CHANNELOUT, "    pixel size    : %d\n", m_pixelSize);
    ThreadedChannelOutputBase::DumpConfig();
}

