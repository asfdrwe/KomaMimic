# Playing videos

[日本語](../../user/video.md) | English

Local videos open in video mode. Images, archives, documents, and videos use the existing list order and adjacent-file navigation. The Video menu is next to Tools.

| Operation | Initial key | Reading-mode comparison |
|---|---|---|
| Play / pause | Space | Space toggles automatic spreads when reading |
| Normal seek backward / forward | Left / Right | Previous / next display |
| Volume | Up / Down | Reading uses these keys for single-page movement |
| Small / large seek | Unassigned | Small seek corresponds to single-page movement |
| Previous / next source | Shift+Left / Right | Shared with reading |
| Return to covers | Shift+Down | Stops video and audio |
| Fit / 100% / zoom | X / C / numeric keypad +/- | Shared reading assignments |
| Regenerate cover | F6 | Uses the saved video track and time |

Small, normal, and large seeks initially use 3, 10, and 60 seconds. Change these in Video preferences. Seeking preserves pause. Frame-accurate stepping is not provided.

Select speed, video/audio tracks, embedded subtitles, aspect ratio, and deinterlacing in the menu. Their cycling commands, A-B commands, and speed adjustments initially have no shortcuts. Assign them in shortcut settings. Speed adjustment selects the next or previous configured speed and stops at the ends.

Subtitle cycling includes Hidden. Video/audio cycling selects one track at a time. TS program grouping is not available. Aspect ratio choices are From video, 1:1, 4:3, and 16:9; 1:1 means square display, independently of 100% scale.

Deinterlacing choices are Off, Blend, Bob (line duplication), and Linear (interpolation). Manually select top field first (TFF) or bottom field first (BFF). Cycling methods preserves field order. A separate command toggles TFF/BFF.

Set A and B, then enable A-B looping. A must precede B. Disabling preserves the points; Clear removes them. Normal looping settings remain saved. Seeking outside A-B returns to A by default, or optionally disables the loop and seeks to the requested position. Stop preserves the loop and points; playback then begins at A.

Autoplay and saved-position resume are independent preferences, both initially enabled. Completed videos reopen at the beginning. Minimizing normally keeps playback running. Optional minimize-pause resumes only if minimizing caused the pause.

The default cover uses the first displayable frame. Registering the current frame includes display processing but excludes subtitles and controls. Saved track/time selections survive cache deletion. Regeneration uses current settings and may look different.

Video decoding offers Automatic (default) and Software. Saving a changed policy displays a restart notice. The running process retains its policy, and saved covers are not regenerated simply because this preference changed.

Format and codec support depend on the bundled Qt Multimedia runtime. Animated images use reading mode by default; a global preference opens Qt-supported formats in video mode. Unknown duration or unavailable seeking limits the applicable controls. Some formats fail to redisplay an ongoing subtitle after seeking because Qt does not re-emit it.

Bitmap subtitles, full ASS styling, external subtitles, chapters, network playback, and video script extensions are outside the initial scope.
