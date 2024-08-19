// GB Enhanced+ Copyright Daniel Baxter 2023
// Licensed under the GPLv2
// See LICENSE.txt for full license text

// File : nmp.cpp
// Date : October 27, 2023
// Description : Nintendo MP3 Player
//
// Handles I/O for the Nintendo MP3 Player
// Manages IRQs and firmware reads/writes
// Play-Yan and Play-Yan Micro handled separately (see play_yan.cpp)

#include "mmu.h"
#include "common/util.h" 

/****** Writes to Nintendo MP3 Player I/O ******/
void AGB_MMU::write_nmp(u32 address, u8 value)
{
	//std::cout<<"PLAY-YAN WRITE -> 0x" << address << " :: 0x" << (u32)value << "\n";

	switch(address)
	{
		//Device Control
		case PY_NMP_CNT:
			play_yan.access_mode &= ~0xFF00;
			play_yan.access_mode |= (value << 8);
			break;

		//Device Control
		case PY_NMP_CNT+1:
			play_yan.access_mode &= ~0xFF;
			play_yan.access_mode |= value;

			//After firmware is loaded, the Nintendo MP3 Player generates a Game Pak IRQ.
			//Confirm firmware is finished with this write after booting.
			if((play_yan.access_mode == 0x0808) && (play_yan.op_state == PLAY_YAN_INIT))
			{
				play_yan.irq_delay = 30;
				play_yan.op_state = PLAY_YAN_BOOT_SEQUENCE;
			}

			//Terminate command input now. Actual execution happens later.
			//Command is always first 16-bits of stream
			else if((play_yan.access_mode == 0x0404) && (play_yan.op_state == PLAY_YAN_PROCESS_CMD))
			{
				if(play_yan.command_stream.size() >= 2)
				{
					play_yan.cmd = (play_yan.command_stream[0] << 8) | play_yan.command_stream[1];
					process_nmp_cmd();
				}
			}

			break;

		//Device Parameter
		case PY_NMP_PARAMETER:
			play_yan.access_param &= ~0xFF00;
			play_yan.access_param |= (value << 8);
			break;

		//Device Parameter
		case PY_NMP_PARAMETER+1:
			play_yan.access_param &= ~0xFF;
			play_yan.access_param |= value;

			//Set high 16-bits of the param or begin processing commands now
			if(play_yan.access_mode == 0x1010) { play_yan.access_param <<= 16; }
			else if(play_yan.access_mode == 0) { access_nmp_io(); }

			break;

		//Device Data Input (firmware, commands, etc?)
		case PY_NMP_DATA_IN:
		case PY_NMP_DATA_IN+1:
			if(play_yan.firmware_addr)
			{
				play_yan.firmware[play_yan.firmware_addr++] = value;
			}

			else if(play_yan.op_state == PLAY_YAN_PROCESS_CMD)
			{
				play_yan.command_stream.push_back(value);
			}

			break;
	}	
}

/****** Reads from Nintendo MP3 Player I/O ******/
u8 AGB_MMU::read_nmp(u32 address)
{
	u8 result = 0;

	switch(address)
	{
		case PY_NMP_DATA_OUT:
		case PY_NMP_DATA_OUT+1:

			//Return SD card data
			if(play_yan.op_state == PLAY_YAN_GET_SD_DATA)
			{
				if(play_yan.nmp_data_index < play_yan.card_data.size())
				{
					result = play_yan.card_data[play_yan.nmp_data_index++];
				}
			}

			//Some kind of status data read after each Game Pak IRQ
			else if(play_yan.nmp_data_index < 16)
			{
				result = play_yan.nmp_status_data[play_yan.nmp_data_index++];
			}

			break;
	}

	//std::cout<<"PLAY-YAN READ -> 0x" << address << " :: 0x" << (u32)result << "\n";

	return result;
}

/****** Handles Nintendo MP3 Player command processing ******/
void AGB_MMU::process_nmp_cmd()
{
	std::cout<<"CMD -> 0x" << play_yan.cmd << "\n";

	//Set up default status data
	for(u32 x = 0; x < 16; x++) { play_yan.nmp_status_data[x] = 0; }
	play_yan.nmp_status_data[0] = (play_yan.cmd >> 8);
	play_yan.nmp_status_data[1] = (play_yan.cmd & 0xFF);

	switch(play_yan.cmd)
	{
		//Start list of files and folders
		case NMP_START_FILE_LIST:
			play_yan.nmp_cmd_status = NMP_START_FILE_LIST | 0x4000;
			play_yan.nmp_valid_command = true;

			play_yan.nmp_status_data[2] = 0;
			play_yan.nmp_status_data[3] = 0;

			play_yan.nmp_entry_count = 0;
			play_yan.music_files.clear();
			play_yan.folders.clear();

			//Grab all folder, then files
			util::get_folders_in_dir(play_yan.current_dir, play_yan.folders);
			util::get_files_in_dir(play_yan.current_dir, ".mp3", play_yan.music_files, false, false);

			//Stop list if done
			if(play_yan.nmp_entry_count >= (play_yan.music_files.size() + play_yan.folders.size()))
			{
				play_yan.nmp_status_data[2] = 0;
				play_yan.nmp_status_data[3] = 1;	
			}

			play_yan.nmp_entry_count++;

			break;

		//Continue list of files and folders
		case NMP_CONTINUE_FILE_LIST:
			play_yan.nmp_cmd_status = NMP_CONTINUE_FILE_LIST | 0x4000;
			play_yan.nmp_valid_command = true;

			//Stop list if done
			if(play_yan.nmp_entry_count >= (play_yan.music_files.size() + play_yan.folders.size()))
			{
				play_yan.nmp_status_data[2] = 0;
				play_yan.nmp_status_data[3] = 1;
			}

			play_yan.nmp_entry_count++;

			break;

		//Change directory
		case NMP_SET_DIR:
			play_yan.nmp_cmd_status = NMP_SET_DIR | 0x4000;
			play_yan.nmp_valid_command = true;

			{
				std::string new_dir = "";

				//Grab directory
				for(u32 x = 3; x < play_yan.command_stream.size(); x += 2)
				{
					u8 chr = play_yan.command_stream[x];
					if(!chr) { break; }

					if((x == 3) && ((chr == 0x01) || (chr == 0x02))) { continue; }
					else { new_dir += chr; }
				}

				//Move one directory up
				if(new_dir == "..")
				{
					u8 chr = play_yan.current_dir.back();

					while(chr != 0x2F)
					{
						play_yan.current_dir.pop_back();
						chr = play_yan.current_dir.back();
					}

					if(chr == 0x2F) { play_yan.current_dir.pop_back(); }
				}

				//Jump down into new directory
				else if(!new_dir.empty())
				{
					play_yan.current_dir += ("/" + new_dir);
				}
			}
				
			break;

		//Get ID3 Tags
		case NMP_GET_ID3_DATA:
			play_yan.nmp_cmd_status = NMP_GET_ID3_DATA | 0x4000;
			play_yan.nmp_valid_command = true;

			//Get music file
			play_yan.current_music_file = "";

			for(u32 x = 3; x < play_yan.command_stream.size(); x += 2)
			{
				u8 chr = play_yan.command_stream[x];
				if(!chr) { break; }

				if((x == 3) && ((chr == 0x01) || (chr == 0x02))) { continue; }
				else { play_yan.current_music_file += chr; }
			}

			//This first time around, this command returns an arbitrary 16-bit value in status data
			//This indicates the 16-bit access index ID3 data can be read from
			//Here, GBE+ forces 0x0101, since the NMP uses that for subsequent ID3 reads anyway
			play_yan.nmp_status_data[6] = 0x1;
			play_yan.nmp_status_data[7] = 0x1;

			play_yan_get_id3_data(play_yan.current_dir + "/" + play_yan.current_music_file);
			play_yan.nmp_title = util::make_ascii_printable(play_yan.nmp_title);
			play_yan.nmp_artist = util::make_ascii_printable(play_yan.nmp_artist);

			break;

		//Play Music File
		case NMP_PLAY_MUSIC:
			play_yan.nmp_cmd_status = NMP_PLAY_MUSIC | 0x4000;
			play_yan.nmp_valid_command = true;
			play_yan.is_music_playing = true;
			play_yan.is_media_playing = true;

			play_yan.audio_sample_index = 0;
			play_yan.l_audio_dither_error = 0;
			play_yan.r_audio_dither_error = 0;
			play_yan.tracker_update_size = 0;
			apu_stat->ext_audio.last_pos = 0;
			apu_stat->ext_audio.sample_pos = 0;

			play_yan.nmp_seek_pos = 0;
			play_yan.nmp_seek_dir = 0xFF;
			play_yan.nmp_seek_count = 0;

			if(apu_stat->ext_audio.use_headphones)
			{
				play_yan.update_audio_stream = false;
				play_yan.update_trackbar_timestamp = true;
				play_yan.nmp_manual_cmd = 0x8100;
				play_yan.irq_delay = 10;
			}
			
			else
			{
				play_yan.update_audio_stream = true;
				play_yan.update_trackbar_timestamp = false;
			}

			//Get music file
			play_yan.current_music_file = "";

			for(u32 x = 3; x < play_yan.command_stream.size(); x += 2)
			{
				u8 chr = play_yan.command_stream[x];
				if(!chr) { break; }

				if((x == 3) && ((chr == 0x01) || (chr == 0x02))) { continue; }
				else { play_yan.current_music_file += chr; }
			}

			if(!play_yan_load_audio(play_yan.current_dir + "/" + play_yan.current_music_file))
			{
				//If no audio could be loaded, use dummy length for song
				play_yan.music_length = 2;
			}

			play_yan.cycles = 0;

			break;

		//Stop Music Playback
		case NMP_STOP_MUSIC:
			play_yan.nmp_cmd_status = NMP_STOP_MUSIC | 0x4000;
			play_yan.nmp_valid_command = true;
			play_yan.is_music_playing = false;
			play_yan.is_media_playing = false;
			apu_stat->ext_audio.playing = false;

			play_yan.audio_frame_count = 0;
			play_yan.tracker_update_size = 0;
			
			play_yan.update_audio_stream = false;
			play_yan.update_trackbar_timestamp = false;

			play_yan.nmp_seek_pos = 0;
			play_yan.nmp_seek_dir = 0xFF;
			play_yan.nmp_seek_count = 0;

			play_yan.nmp_manual_cmd = 0;
			play_yan.irq_delay = 0;
			play_yan.last_delay = 0;
			play_yan.nmp_manual_irq = false;

			break;

		//Pause Music Playback
		case NMP_PAUSE:
			play_yan.nmp_cmd_status = NMP_PAUSE | 0x4000;
			play_yan.nmp_valid_command = true;
			play_yan.is_music_playing = false;
			play_yan.is_media_playing = false;
			apu_stat->ext_audio.playing = false;

			play_yan.nmp_seek_pos = 0;
			play_yan.nmp_seek_dir = 0xFF;
			play_yan.nmp_seek_count = 0;

			play_yan.last_delay = play_yan.irq_delay;
			play_yan.nmp_manual_cmd = 0;
			play_yan.irq_delay = 0;
			play_yan.nmp_manual_irq = false;

			break;

		//Resume Music Playback
		case NMP_RESUME:
			play_yan.nmp_cmd_status = NMP_PAUSE | 0x4000;
			play_yan.nmp_valid_command = true;
			play_yan.is_music_playing = true;
			play_yan.is_media_playing = true;

			if(play_yan.audio_sample_rate && play_yan.audio_channels)
			{
				apu_stat->ext_audio.playing = true;
			}

			if(apu_stat->ext_audio.use_headphones)
			{
				play_yan.update_audio_stream = false;
				play_yan.update_trackbar_timestamp = true;

				play_yan.nmp_manual_cmd = NMP_UPDATE_AUDIO;
				play_yan.irq_delay = play_yan.last_delay;
				play_yan.last_delay = 0;
			}
			
			else
			{
				play_yan.update_audio_stream = true;
				play_yan.update_trackbar_timestamp = false;
			}

			break;

		//Seek Forwards/Backwards
		case NMP_SEEK:
			play_yan.nmp_cmd_status = NMP_SEEK | 0x4000;
			play_yan.nmp_valid_command = true;

			if(play_yan.command_stream.size() >= 4)
			{
				play_yan.nmp_seek_count++;
				s32 seek_shift = 2 + (play_yan.nmp_seek_count / 10); 

				//Wait until at least two inputs from this command are non-zero
				if(play_yan.nmp_seek_dir == 0xFF)
				{
					u8 last_pos = play_yan.nmp_seek_pos;
					play_yan.nmp_seek_pos = play_yan.command_stream[3];

					if(last_pos && play_yan.nmp_seek_pos)
					{
						//Rewind = inputs decrements
						if(play_yan.nmp_seek_pos < last_pos) { play_yan.nmp_seek_dir = 0; }

						//Fast Forward = inputs increment
						else { play_yan.nmp_seek_dir = 1; }
					}
				}

				//Rewind playback position
				else if(!play_yan.nmp_seek_dir)
				{
					if(apu_stat->ext_audio.use_headphones)
					{
						s32 new_pos = apu_stat->ext_audio.sample_pos - (play_yan.audio_sample_rate * seek_shift);
						if(new_pos < 0) { new_pos = 0; }

						apu_stat->ext_audio.sample_pos = new_pos;
					}

					else
					{
						s32 new_pos = play_yan.audio_sample_index - (16384 * seek_shift);
						if(new_pos < 0) { new_pos = 0; }

						play_yan.audio_sample_index = new_pos;
					}
				}

				//Fast forward playback position
				else
				{
					if(apu_stat->ext_audio.use_headphones) { apu_stat->ext_audio.sample_pos += (play_yan.audio_sample_rate * seek_shift); }
					else { play_yan.audio_sample_index += (16384 * seek_shift); }
				}

				play_yan.nmp_manual_cmd = NMP_UPDATE_AUDIO;
				play_yan.update_audio_stream = false;
				play_yan.update_trackbar_timestamp = true;
				play_yan.irq_delay = 0;
				play_yan.nmp_manual_irq = true;
				process_play_yan_irq();
				play_yan.nmp_manual_irq = false;
			}

			break;

		//Adjust Volume - No IRQ generated
		case NMP_SET_VOLUME:
			if(play_yan.command_stream.size() >= 4)
			{
				play_yan.volume = play_yan.command_stream[3];
				apu_stat->ext_audio.volume = (play_yan.volume / 46.0) * 63.0;
			}

			//Reset seek data
			play_yan.nmp_seek_pos = 0;
			play_yan.nmp_seek_dir = 0xFF;

			break;

		//Generate Sound (for menus) - No IRQ generated
		case NMP_PLAY_SFX:
			play_yan.nmp_valid_command = true;
			play_yan.is_music_playing = true;
			play_yan.is_media_playing = true;

			play_yan.audio_sample_index = 0;
			play_yan.l_audio_dither_error = 0;
			play_yan.r_audio_dither_error = 0;
			play_yan.tracker_update_size = 0;
			apu_stat->ext_audio.last_pos = 0;
			apu_stat->ext_audio.sample_pos = 0;

			play_yan.update_audio_stream = true;
			play_yan.update_trackbar_timestamp = false;

			//Get SFX file
			{
				std::string sfx_file = config::data_path + "play_yan/sfx.wav";
				play_yan_load_audio(sfx_file);
			}

			play_yan.nmp_manual_cmd = NMP_UPDATE_AUDIO;
			play_yan.nmp_manual_irq = true;
			process_play_yan_irq();
			play_yan.nmp_manual_irq = false;

			break;

		//Check for firmware update file (presumably)
		case NMP_CHECK_FIRMWARE_FILE:
			play_yan.nmp_cmd_status = NMP_CHECK_FIRMWARE_FILE | 0x4000;
			play_yan.nmp_valid_command = true;
			
			break;

		//Unknown command (firmware update related? presumably, read firmware)
		case NMP_READ_FIRMWARE_FILE:
			play_yan.nmp_cmd_status = NMP_READ_FIRMWARE_FILE | 0x4000;
			play_yan.nmp_valid_command = true;
			
			break;

		//Unknown command (firmware update related? presumably, close firmware file)
		case NMP_CLOSE_FIRMWARE_FILE:
			play_yan.nmp_cmd_status = NMP_CLOSE_FIRMWARE_FILE | 0x4000;
			play_yan.nmp_valid_command = true;
			play_yan.cmd = 0;
			
			break;

		//Sleep Start
		case NMP_SLEEP:
			play_yan.nmp_cmd_status = NMP_SLEEP | 0x8000;
			play_yan.nmp_valid_command = true;

			break;

		//Sleep End
		case NMP_WAKE:
			play_yan.nmp_cmd_status = NMP_WAKE | 0x8000;
			play_yan.nmp_valid_command = true;

			break;

		//Init NMP Hardware
		case NMP_INIT:
			play_yan.nmp_cmd_status = NMP_INIT;
			play_yan.nmp_valid_command = true;
			
			break;

		//Continue music stream
		case NMP_UPDATE_AUDIO:
			play_yan.nmp_cmd_status = NMP_UPDATE_AUDIO;
			play_yan.nmp_valid_command = false;
			play_yan.nmp_data_index = 0;

			//Trigger additional IRQs for processing music
			if(play_yan.is_music_playing)
			{
				play_yan.nmp_manual_cmd = NMP_UPDATE_AUDIO;
				play_yan.audio_buffer_size = 0x480;

				//Prioritize audio stream updates
				if(play_yan.update_audio_stream && !apu_stat->ext_audio.use_headphones)
				{
					//Audio buffer size (max 0x480), *MUST* be a multiple of 16!
					play_yan.nmp_status_data[2] = (play_yan.audio_buffer_size >> 8);
					play_yan.nmp_status_data[3] = (play_yan.audio_buffer_size & 0xFF);

					//SD Card access ID - Seems arbitrary, so forced to 0x0202 here
					play_yan.nmp_status_data[4] = 0x02;
					play_yan.nmp_status_data[5] = 0x02;

					play_yan.nmp_audio_index = 0x202 + (play_yan.audio_buffer_size / 4);
				}

				else if(play_yan.update_trackbar_timestamp)
				{
					play_yan.update_audio_stream = true;
					play_yan.update_trackbar_timestamp = false;
					play_yan.audio_frame_count = 0;

					u32 current_sample_pos = (apu_stat->ext_audio.use_headphones) ? apu_stat->ext_audio.sample_pos : play_yan.audio_sample_index;
					u32 current_sample_rate = (apu_stat->ext_audio.use_headphones) ? play_yan.audio_sample_rate : 16384;

					if(current_sample_rate)
					{
						play_yan.tracker_update_size = (current_sample_pos / current_sample_rate);
					}

					//Trackbar position - 0 to 99
					if(play_yan.music_length - 1)
					{
						float progress = play_yan.tracker_update_size;

						progress /= play_yan.music_length - 1;
						progress *= 100.0;

						play_yan.nmp_status_data[8] = u8(progress);

						if(progress >= 100)
						{
							play_yan.nmp_manual_cmd = NMP_STOP_MUSIC;
							play_yan.irq_delay = 1;
							break;
						}
					}

					//Song timestamp in seconds
					//Treated here as a 24-bit MSB value, with bytes 15, 12, and 13 (in that order)
					//play_yan.nmp_status_data[15] = (play_yan.tracker_update_size >> 16) & 0xFF;
					play_yan.nmp_status_data[12] = (play_yan.tracker_update_size >> 8) & 0xFF;
					play_yan.nmp_status_data[13] = (play_yan.tracker_update_size & 0xFF);

					if(apu_stat->ext_audio.use_headphones)
					{
						play_yan.irq_delay = 60;
						play_yan.update_audio_stream = false;
						play_yan.update_trackbar_timestamp = true;
					}
				}

				//Start external audio output
				if(!apu_stat->ext_audio.playing && play_yan.audio_sample_rate && play_yan.audio_channels)
				{
					apu_stat->ext_audio.channels = play_yan.audio_channels;
					apu_stat->ext_audio.frequency = play_yan.audio_sample_rate;
					apu_stat->ext_audio.sample_pos = 0;
					apu_stat->ext_audio.playing = true;
				}
			}

			break;

		//Headphone Status
		case NMP_HEADPHONE_STATUS:
			play_yan.nmp_cmd_status = NMP_HEADPHONE_STATUS;
			play_yan.nmp_valid_command = true;

			apu_stat->ext_audio.use_headphones = !apu_stat->ext_audio.use_headphones;

			//Switch between headphone and GBA speaker output
			if(apu_stat->ext_audio.use_headphones)
			{
				play_yan.nmp_status_data[2] = 0;
				play_yan.nmp_status_data[3] = 1;

				play_yan.update_audio_stream = false;
				play_yan.update_trackbar_timestamp = true;

				if(play_yan.audio_channels)
				{
					u32 index = apu_stat->ext_audio.last_pos / play_yan.audio_channels;

					apu_stat->ext_audio.sample_pos = index;
				}

				//Force timestamp update after switching to headphones
				if(apu_stat->ext_audio.playing)
				{
					play_yan.nmp_manual_cmd = NMP_UPDATE_AUDIO;
					play_yan.irq_delay = 1;
				}
			}

			else
			{
				play_yan.update_audio_stream = true;
				play_yan.update_trackbar_timestamp = false;

				if(play_yan.audio_channels)
				{
					double ratio = play_yan.audio_sample_rate / 16384.0;
					u32 index = apu_stat->ext_audio.last_pos / play_yan.audio_channels;
					index /= ratio;

					play_yan.audio_sample_index = (index & ~0x01);
				}

				play_yan.nmp_manual_cmd = 0;
				play_yan.irq_delay = 0;
			}

			break;

		default:
			play_yan.nmp_valid_command = false;
			play_yan.nmp_cmd_status = 0;
			std::cout<<"Unknown Nintendo MP3 Player Command -> 0x" << play_yan.cmd << "\n";
	}
}

/****** Handles prep work for accessing Nintendo MP3 Player I/O such as writing commands, cart status, busy signal etc ******/
void AGB_MMU::access_nmp_io()
{
	play_yan.firmware_addr = 0;

	//Determine which kinds of data to access (e.g. cart status, hardware busy flag, command stuff, etc)
	if((play_yan.access_param) && (play_yan.access_param != 0x101) && (play_yan.access_param != 0x202) && (play_yan.access_param != play_yan.nmp_audio_index))
	{
		//std::cout<<"ACCESS -> 0x" << play_yan.access_param << "\n";
		play_yan.firmware_addr = (play_yan.access_param << 1);

		u16 stat_data = 0;

		//Cartridge Status
		if(play_yan.access_param == 0x100)
		{
			//Cartridge status during initial boot phase (e.g. Health and Safety screen)
			if(play_yan.nmp_init_stage < 4)
			{
				stat_data = play_yan.nmp_boot_data[play_yan.nmp_init_stage >> 1];
				play_yan.nmp_init_stage++;

				if(play_yan.nmp_init_stage == 2) { memory_map[REG_IF+1] |= 0x20; }
			}

			//Status after running a command
			else if(play_yan.nmp_cmd_status)
			{
				stat_data = play_yan.nmp_cmd_status;
			}
		}

		//Write command or wait for command to finish
		else if(play_yan.access_param == 0x10F)
		{
			play_yan.op_state = PLAY_YAN_PROCESS_CMD;
			play_yan.firmware_addr = 0;
			play_yan.command_stream.clear();

			//Finish command
			if(play_yan.nmp_valid_command)
			{
				memory_map[REG_IF+1] |= 0x20;
				play_yan.nmp_valid_command = false;
			}

			//Increment internal ticks
			//Value here is 6 ticks, a rough average of how often a real NMP updates at ~60Hz
			play_yan.nmp_ticks += 6;
			stat_data = play_yan.nmp_ticks;
		}

		//I/O Busy Flag
		//Signals the end of a command
		else if(play_yan.access_param == 0x110)
		{
			//1 = I/O Busy, 0 = I/O Ready. For now, we are never busy
			play_yan.op_state = PLAY_YAN_WAIT;
		}

		play_yan.nmp_status_data[0] = (stat_data >> 8);
		play_yan.nmp_status_data[1] = (stat_data & 0xFF);
		play_yan.nmp_data_index = 0;
		play_yan.access_param = 0;
	}

	//Access SD card data
	else
	{
		play_yan.card_data.clear();
		play_yan.op_state = PLAY_YAN_GET_SD_DATA;

		switch(play_yan.cmd)
		{
			//File and folder list
			case NMP_START_FILE_LIST:
			case NMP_CONTINUE_FILE_LIST:
				play_yan.nmp_data_index = 0;
				play_yan.card_data.resize(528, 0x00);

				if(play_yan.nmp_entry_count)
				{
					std::string list_entry = "";
					bool is_folder = false;
					u32 folder_limit = play_yan.folders.size();
					u32 real_entry = play_yan.nmp_entry_count - 1;
					
					if(real_entry < folder_limit)
					{
						list_entry = play_yan.folders[real_entry];
						is_folder = true;
					}

					else
					{
						real_entry -= folder_limit;
						list_entry = play_yan.music_files[real_entry];
					}

					u32 str_len = (list_entry.length() > 255) ? 255 : list_entry.length();

					//Sort folders first. Use lower unprintable non-zero character as first character
					if(is_folder)
					{
						play_yan.card_data[0] = 0x00;
						play_yan.card_data[1] = 0x01;
					}

					else
					{
						play_yan.card_data[0] = 0x00;
						play_yan.card_data[1] = 0x02;
					}

					for(u32 x = 0; x < str_len; x++)
					{
						u8 chr = list_entry[x];
						play_yan.card_data[(x * 2) + 2] = 0x00;
						play_yan.card_data[(x * 2) + 3] = chr;
					}

					//Set file/folder flag expected by NMP. 0x01 = Folder, 0x02 = File
					play_yan.card_data[525] = (is_folder) ? 0x01 : 0x02;
				}

				break;

			//ID3 Data
			case NMP_GET_ID3_DATA:
				play_yan.nmp_data_index = 0;
				play_yan.card_data.resize(272, 0x00);

				{
					u32 id3_pos = 4;

					u32 str_len = (play_yan.nmp_title.length() > 66) ? 66 : play_yan.nmp_title.length();

					for(u32 x = 0; x < str_len; x++)
					{
						u8 chr = play_yan.nmp_title[x];
						play_yan.card_data[id3_pos++] = 0x00;
						play_yan.card_data[id3_pos++] = chr;
					}

					id3_pos = 136;

					str_len = (play_yan.nmp_artist.length() > 68) ? 68 : play_yan.nmp_artist.length();

					for(u32 x = 0; x < str_len; x++)
					{
						u8 chr = play_yan.nmp_artist[x];
						play_yan.card_data[id3_pos++] = 0x00;
						play_yan.card_data[id3_pos++] = chr;
					}
				}

				break;

			//Music Data
			case NMP_UPDATE_AUDIO:
				if(play_yan.update_audio_stream)
				{
					play_yan.card_data.resize(play_yan.audio_buffer_size + 2, 0x00);
					play_yan.nmp_data_index = 0;
					play_yan.audio_frame_count++;

					bool trigger_timestamp = false;

					if(play_yan.audio_sample_rate)
					{
						double ratio = play_yan.audio_sample_rate / 16384.0;
						s16* e_stream = (s16*)apu_stat->ext_audio.buffer;
						u32 stream_size = apu_stat->ext_audio.length / 2;

						bool is_left_channel = (play_yan.audio_frame_count & 0x01) ? true : false;

						//Trigger timestamp update early when first playing song
						if(!play_yan.audio_sample_index && !is_left_channel) { trigger_timestamp = true; }

						s32 sample = 0;
						s16 error = 0;
						u32 index = 0;
						u32 index_shift = (is_left_channel) ? 0 : 1;
						u32 sample_count = 0;
						u32 offset = 0;
						u32 limit = (play_yan.audio_buffer_size / 2) + 2;

						for(u32 x = 2; x < limit; x++)
						{
							error = (is_left_channel) ? play_yan.l_audio_dither_error : play_yan.r_audio_dither_error;

							index = (ratio * play_yan.audio_sample_index);
							index *= play_yan.audio_channels;
							index += index_shift;

							if(index >= stream_size)
							{
								index = (stream_size - 1);
								play_yan.is_music_playing = false;
								play_yan.is_media_playing = false;
							}

							//Perform simple Flyod-Steinberg dithering
							//Grab current sample and add 7/16 of error, quantize results, clip results 
							sample = e_stream[index];
							sample += ((error >> 4) * 7);
							sample >>= 8;

							if(sample > 127) { sample = 127; }
							else if(sample < -128) { sample = -128; }

							//Calculate new error
							error = e_stream[index] & 0xFF;

							if(is_left_channel) { play_yan.l_audio_dither_error = error; }
							else { play_yan.r_audio_dither_error = error; }

							//Output new samples
							offset = (play_yan.audio_sample_index & 0x01) ? (x - 1) : (x + 1);
							play_yan.card_data[offset] = (sample & 0xFF);
							play_yan.audio_sample_index++;
							sample_count++;

							//Trigger timestamp update periodically, use samples to count seconds
							if(((play_yan.audio_sample_index % 16384) == 0) && (!is_left_channel))
							{
								trigger_timestamp = true;
							}
						}

						if(is_left_channel) { play_yan.audio_sample_index -= sample_count; }
						else { apu_stat->ext_audio.last_pos = index; }
					}

					if(trigger_timestamp)
					{
						play_yan.update_audio_stream = false;
						play_yan.update_trackbar_timestamp = true;
						play_yan.irq_delay = 0;
						play_yan.nmp_manual_irq = true;
						process_play_yan_irq();
						play_yan.nmp_manual_irq = false;
					}
				}

				break;
		}
	}
}
