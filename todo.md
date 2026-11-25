Normally I want the cursor at the beginning of the buffer, because
normally I want to read the whole thing; I'll see if this is the right
place to put it after inserting a file.  Yes, it's fine. 

Maybe in the future I'll put in a feature where the program tracks
the location of each text element (command line file entries, stdin,
insertions) and lets the user go to a given spot.

Makes sense. For the future "jump to element" feature, you could keep a
simple array of `{offset, label}` pairs — one per command-line file,
one for stdin, one per insertion — and expose it as a menu item or a
`Ctrl+G`-style goto dialog. Since you already have `rebuild_lines()`
maintaining line offsets, the labels would just be byte offsets into
`text` that you map to line numbers with `line_of()`. The tricky part
is that edits (inserts/deletes) shift subsequent offsets, so you'd need
to adjust the array on every mutation — or alternatively store them
as line numbers and re-derive byte offsets from `lines[]` each time,
which is more robust against edits.

Right-click floating Edit menu doesn't work with Search bar.

Also Cancel / OK on the file picker.
