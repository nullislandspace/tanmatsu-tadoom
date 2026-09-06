## Progress Tracker

### Phase 1: Scaffolding
- [x] Create main/prboom/ directory, copy core sources
- [x] Create main/lib/tsf.h
- [x] Create main/config.h
- [x] Update main/CMakeLists.txt
- [x] Update top-level CMakeLists.txt
- [x] Update main/idf_component.yml
- [x] Update sdkconfigs/tanmatsu
- [x] Add sdcard.c/.h, fastopen.c/.h

### Phase 2: HAL Implementation
- [x] main/main.c (entry point)
- [x] main/i_system_tanmatsu.c (timing, file I/O)
- [x] main/i_video_tanmatsu.c (display + input)
- [x] main/i_sound_tanmatsu.c (SFX + music)
- [x] main/midi_player.c/.h (MIDI sequencer)
- [x] main/i_joy_tanmatsu.c (USB HID gamepad)
- [x] main/i_network_tanmatsu.c (stub)

### Phase 3: Source Patches
- [x] doomtype.h boolean fix
- [x] lprintf.c cleanup (remove isatty/Win32)
- [x] config.h: PACKEDATTR, HAVE_STRLWR, HAVE_OWN_MUSIC
- [x] Force-include config.h on all prboom sources

### Phase 4: Testing
- [x] M1: Compiles
- [~] M2: Title screen (shows but rotated 90° CCW, rotation fix in progress)
- [~] M3: Menu navigation (works if you press Esc quickly during demo)
- [~] M4: Gameplay (new game works, demo playback crashes in R_CachePatchNum)
- [ ] M5: Sound effects (disabled via -nosound for debugging)
- [ ] M6: Music
- [ ] M7: Performance
- [ ] M8: Polish

### Known Issues
- **BSS corruption during R_Init**: visplanes[] array overwritten with what looks like
  RISC-V machine code during R_InitPatches. Worked around with R_ResetVisplanes()
  call after R_Init. Root cause unknown (possibly cache/DMA related on ESP32-P4).
- **Demo crash**: R_CachePatchNum crashes with load access fault during demo playback.
  Starting a new game works fine. May be related to broader BSS corruption.
- **Display rotation**: Panel reports 480x800 (portrait). Rotation code added in
  i_video_tanmatsu.c but not yet verified on hardware.
- **Main task stack**: Reduced to 8192; doom task stack allocated from PSRAM via
  xTaskCreateStaticPinnedToCore (TCB in internal RAM, stack in PSRAM).
- **SPIRAM_MALLOC_RESERVE_INTERNAL**: Reduced from 64KB to 32KB to fit in available
  internal RAM.
