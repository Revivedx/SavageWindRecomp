# Software keyboard output

The overlay edits UTF-8 text. The `nn::swkbd::ShowKeyboard` HLE returns it using the encoding selected by
`ShowKeyboardArg + 0x3bd` (`IsUseUtf8`): UTF-8 when nonzero, UTF-16LE when zero.
BotW 1.9.0's horse-naming caller leaves this flag zero and reads the result as 16-bit units.

The output `String` contains a pointer and a byte capacity, including space for the terminator. Conversion
keeps complete characters, reserves a one-byte or two-byte terminator, and never splits a UTF-8 sequence
or UTF-16 surrogate pair at the buffer limit. Invalid UTF-8 input bytes become U+FFFD.

Run the regression checks with:

```sh
make test-swkbd
```

The test calls the actual HLE with synthetic guest buffers and a replacement UI prompt. It needs no game
files, renderer or window. Cases include the reported `Talon`, odd/even and nine-letter ASCII names, both
output encodings, Unicode boundary values, surrogate pairs, malformed input, repeated prompts, terminators
and short/odd byte capacities with output guards.

For a live check, register a horse, confirm its full name in the dialogue and horse list, then save and reload.
Check both physical keyboard and controller entry. This exercises the game and UI beyond the synthetic prompt.
