# Ember — video script

About ten minutes. Narration in plain text, shot directions in *[brackets]*.
Section timings are a guide for the edit, not a metronome.

Facts checked against the code and the boot logs as of build 44 (6 September 2026).
Anything marked **⚠** is a place where the earlier draft said something inaccurate; the
fix is in the text and the reason is at the end.

---

## 0:00 — Cold open (≈ 1:00)

*[Ember desktop. Drag a window fast. Music playing, analyser bouncing. Tap through the
touchscreen. Cut to Doom. Cut to the Yoga folded flat with the on-screen keyboard up.]*

A few weeks ago this old Lenovo Yoga was collecting dust. Today it's running an operating
system that didn't exist a few weeks ago.

It's called Ember. It boots on real hardware from a USB stick, runs DOS programs, has its
own graphical desktop, sound, a file manager, a music player, a word processor, a paint
program, a touchpad and touchscreen driver — and an on-screen keyboard.

*[Fold the Yoga flat.]*

Which means, because this is a Yoga, my homemade operating system works as a tablet.

I didn't type the code. Claude did. But an AI doesn't decide that a DOS from scratch
should have a music player. It doesn't decide the Start button should be a crystal that
splits open. It doesn't say "no, that looks like Windows 95, make it look like *ours*." It
doesn't think of a paint program for fingers, or a screenshot tool, or a battery in the
bar, or that a Yoga with a working touchscreen is a tablet waiting to happen.

That was me. Every feature you're about to see, I asked for. Every design, I directed.
Every bug, I found on the real machine and sent back. The AI made it real. This is what
that partnership built.

*[Title: EMBER.]*

---

## 1:00 — How it started (≈ 0:40)

*[Old footage or photos: the laptop, the early repo, the NanoDOS name.]*

This started as an experiment. I wanted to see how far Claude could get if I asked for an
operating system from *scratch*.

Not Linux with a new desktop. Not FreeDOS with programs on top. Something that boots
itself, reads its own filesystem, and talks to the hardware directly.

I called it NanoDOS. I didn't expect it to get far.

---

## 1:40 — First boots (≈ 1:00)

*[Phone photo: the scrambled screen.]*

This was the first boot on the real machine.

Not a success. But encouraging — the computer *was* running our code. Just very badly.

*[Live on the laptop: at the DOS prompt, type `WIN`. The teal desktop appears.]*

A few attempts later, this appeared. A crude little Windows-95-looking interface, drawn
by a kernel that was about forty kilobytes of hand-written assembly.

And nothing worked. No mouse, no sound, and the trackpad did absolutely nothing.

So this was never one prompt. It was a loop: build it, burn it, boot it, photograph what
broke, send the photo back, try again. Hundreds of times.

> The old desktop still ships inside the kernel. Boot the current stick, hold Shift at
> "Starting Ember" to stay at the prompt, and type `WIN`. Film the "before" live rather
> than from a screenshot.

---

## 2:40 — Doom (≈ 1:30)

*[DOS prompt. Doom starting.]*

With the keyboard and a USB mouse working, I moved on to the most important feature any
new operating system needs.

*[Beat.]*

Doom.

And this brought back something I'd forgotten: getting a DOS game to run used to be
*work*. Memory managers, conventional memory, drivers. Anyone who had a 486 remembers.

*[Doom running, silent.]*

Eventually Doom ran. One small problem.

*[Hold on silent gameplay.]*

No sound.

The original game wants a Sound Blaster, and the AI told me straight that pretending to
be a Sound Blaster on modern hardware would be a huge job. So we did the *un*sensible
thing: compiled Doom from its source code to run natively on Ember, and then wrote a
driver for the laptop's own HD Audio chip — the same one Windows uses. **⚠**

*[Sound comes in.]*

Which is ridiculous. But it worked.

*[Let the game audio play. Several seconds, no talking.]*

And yes, I bought Doom from GOG for this. About five dollars.

---

## 4:10 — Alley Cat and the PC speaker (≈ 1:00)

*[Alley Cat.]*

Then I tried one of my favourites as a kid. Alley Cat. It ran immediately.

*[Beat.]*

Silently.

Because Alley Cat doesn't use a sound card. It uses the PC speaker — a part this laptop
doesn't have. So Ember had to catch the game's beeps in flight and play them through the
real speakers instead. **⚠**

*[Lockup footage if you have it.]*

This was the most frustrating stretch of the whole project. Weird noises. Freezes. Hard
lockups. Reboot, change something, try again. I don't know how many times.

*[Alley Cat with sound.]*

But eventually — completely playable.

*[Prince of Persia.]*

Prince of Persia just worked.

---

## 5:10 — Ember (≈ 1:10)

*[The teal NanoDOS desktop fades to the amber Ember desktop.]*

The first version had none of this. No sound, no music player, no screenshots, no icons
of its own, a Start menu copied from 1995. Every single thing that makes Ember *Ember*
was something I asked for.

By now this was a lot more than NanoDOS, so it needed a name. An ember is what's left when
a fire has burned down: small, dark on the outside, still glowing, still hot enough to
start a new one. That's this project. DOS is the fire that burned out thirty years ago,
and this is the small glowing piece of it that's still alive — under a megabyte, and
warm.

The name also set the look. I did *not* want another Windows 95 clone. I wanted it to look
like its own thing.

*[The amber desktop. The crystal splitting open into its menu.]*

So this is the design. Amber on black, like a coal in the dark. Windows with real depth.
And instead of a Start button, a crystal at the top that glows like an ember and splits
in half, with the menu sliding down between the halves.

It's still changing every day. But it has real applications now: a file manager, a music
player with a spectrum analyser, a word processor, a paint program built for fingers, a
picture browser, a clock, a calendar, sticky notes, a system monitor.

---

## 6:20 — The touchpad (≈ 1:10)

*[Touchpad working.]*

Remember the touchpad that did nothing? We came back to it, and this is my favourite
detective story from the whole project.

The touchpad isn't a mouse. It's a chip on an I²C bus, hidden behind a controller the
firmware doesn't even list. Ember couldn't see it at all.

So we cheated. I booted the laptop into Windows, and Windows told us everything: the
controller's address in memory, the touchpad's address on the bus, the register that
holds its description.

Then the twist: when Ember boots, the firmware leaves that controller *asleep*. Its
registers read as nothing but ones. We had to find the page of memory where the firmware's
own power-on routine writes, wake the controller ourselves, and only then could Ember
talk to the pad.

*[Finger on the pad, pointer moving. Tap, right-click.]*

And it worked. Taps, clicks, everything.

*[Touchscreen.]*

The touchscreen sat on the second controller, same trick. That worked too.

---

## 7:30 — Tablet mode (≈ 0:40)

*[Fold the Yoga.]*

And once the touchscreen worked I realised: this is a Yoga.

*[Fold it flat.]*

Ember could be a tablet. It just needed a keyboard.

*[Open the on-screen keyboard. Type something visibly.]*

So we made one.

*[Stop talking. Demonstrate: open Paint, draw with a finger, switch brushes, undo. Open
Write, type on the on-screen keyboard.]*

---

## 8:10 — The performance problem (≈ 1:10)

*[Old sluggish footage: dragging a window, the analyser lagging.]*

There was one huge problem. Performance.

Dragging a window was slow. Playing music made it worse. Doing both at once was
apparently too much for a modern computer.

So we built a resource monitor — mostly to show off. And it immediately told us something
we'd have never guessed.

*[Monitor on screen. Point at the line.]*

Drawing a frame took nine milliseconds. *Pushing that frame to the screen* took
thirty-six. Four times longer to copy the picture than to draw it.

The cause was one setting in the CPU. The firmware had marked the graphics memory
*uncacheable*, so every single pixel went across the bus as its own transaction — about
twenty megabytes a second. A real graphics driver sets that memory to *write-combining*,
so the CPU sends pixels in bursts. Ember did the same: rewrote the CPU's memory-type table
at start-up, and put the firmware's table back on exit. **⚠**

*[Before / after.]*

This is before. This is after.

*[Drag windows aggressively while music plays.]*

Same hardware. Same operating system. One setting. Now it flies.

---

## 9:20 — Boot speed and size (≈ 0:50)

*[Laptop fully off. Timer on screen.]*

And while testing all this I noticed something else.

*[Power on.]*

From completely powered off to the DOS prompt: about seventeen seconds — and most of that
is the laptop's own firmware, not Ember.

*[Type EMBER.]*

From the prompt to the full graphical desktop: under two seconds. That's not waking from
sleep. That's the whole desktop starting.

*[File manager showing EMBER.N32.]*

And here's the number that still gets me. The entire desktop — window manager, fonts,
icons, the touch drivers, the music player, the word processor, Paint, the picture
browser, the PNG encoder and decoder, all of it — is *one file*. Six hundred and forty
kilobytes. The kernel underneath it is another forty-five.

Windows 11 is about twenty-five gigabytes. That makes Ember roughly thirty-five thousand
times smaller. Some of that gap is things Ember doesn't have — every driver ever made, a
browser, a security model. But a desktop that draws windows, plays music, edits text and
paints with a finger fits in less than a megabyte. That part of the gap is the
interesting part.

---

## 10:10 — Ending (≈ 0:40)

*[Montage: touchpad, touchscreen, tablet keyboard, Doom, music, the crystal menu, Paint.]*

So this is Ember, right now.

A few weeks ago there was nothing here. Now it has its own boot loader and filesystem,
runs DOS software and native software, plays audio, drives the touchscreen and the
touchpad, works as a tablet, runs Doom — and starts faster than any computer I own.

It's not Linux. It's not Windows. It's not macOS.

Nobody typed this code. But someone had to imagine it, argue with the result, and boot it
four hundred times until it worked. Turns out that's the job now: not writing the
operating system, but deciding what it should be.

*[Beat.]*

An operating system, dreamed up by a person and hallucinated into existence by an AI.

*[Beat.]*

And I'm nowhere near done.

*[Ember splash.]*

---

## Shot list

| Section | Source |
|---|---|
| Scrambled first boot | Phone photos (`screenshot/IMG_65xx.JPG`) |
| Teal NanoDOS desktop | Live: hold Shift at boot, type `WIN` at the prompt. Also `docs/screenshots/start-menu.png`, `low-res-320x200.png` |
| First Doom run, silent | `build/doom1.png` (QEMU, 3 Sep) or re-shoot on the laptop |
| Alley Cat / Prince | Live on the laptop |
| Crystal in its stages | `build/crystal1.png` → `crystal_glow.png` → `crystal_open.png` |
| Touchpad, touchscreen, tablet | Live on the laptop |
| Monitor before / after | Live: Monitor shows "present" time and the framebuffer memory type. Before-footage exists only if you filmed it; a build older than 28 will reproduce the slow behaviour |
| Boot timer | Live, phone stopwatch in frame |
| EMBER.N32 size | Live: Files, root folder |

The `build/` captures are git-ignored and exist only on the development PC. Back them up.

---

## ⚠ What changed from the earlier draft, and why

- **Performance was never caching.** Two real causes: repaints redrew the whole screen for
  every small change (fixed by repainting only the region that changed), and the
  framebuffer's memory type, which the firmware left uncacheable. Ember rewrites the
  CPU's memory-type range registers (MTRRs) so the framebuffer is write-combining, and
  restores the firmware's table on exit. Measured in the Monitor: present time fell from
  about 36 ms per frame to a few milliseconds.
- **"Realtek".** The codec brand was never confirmed from Ember's side; "the laptop's own
  HD Audio chip" is safe and true. Windows Device Manager will give the name if wanted.
- **"Emulating the PC speaker".** There is no speaker to emulate. Ember intercepts the
  speaker programming and plays the tone through the sound chip. "Catch the beeps in
  flight" is accurate.
- **The credit.** The earlier draft said "written by an AI" and left it there. The truth
  is a split: the human chose every feature, directed the design, rejected what looked
  wrong, and ran every hardware test; the AI wrote the code. Both parts are in the script
  now, because the partnership *is* the story.
- **Timeline.** Say whatever is true. The compressed version reads better than an inflated
  one.
- **Boot time.** Most of the seventeen seconds is the firmware. Ember's own share is about
  two seconds to the prompt, which makes the "under two seconds to the desktop" line land.

## Numbers, for the record

| Thing | Value |
|---|---|
| Kernel (16-bit, assembly) | ~45 KB |
| Desktop and every app (EMBER.N32) | ~640–660 KB |
| Windows 11 install | ~25 GB, so roughly 35,000× |
| Frame present, before / after | ~36 ms / a few ms |
| Framebuffer push, uncacheable | ~20 MB/s |
| Touchpad | Synaptics, I²C address 0x2C, HID over I²C |
| Touchscreen | Atmel, I²C address 0x4A, HID over I²C |
| Screen | 3200 × 1800 |
