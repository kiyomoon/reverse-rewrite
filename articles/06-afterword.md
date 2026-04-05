# The Reverse Rewrite — Afterword: Notes on How This Series Got Made

*By Su Qingyue*

---

A few months ago, I had an idea I couldn't let go of: if AI can translate code between languages, then some of the stronger claims around Rust and C++ become empirically checkable. You can at least try the translation in reverse and see what survives.

I thought about it for weeks. Not the execution so much as the framing. Whether the distinction between expressiveness and rejection was real or just a rhetorical convenience. Whether the experiment would actually clarify anything or just generate noise. Whether it was worth doing at all.

Eventually I talked it through with Claude on claude.ai — the project structure, which Rust projects to target, what order to present the material. We went back and forth refining the methodology, choosing hexyl as the first subject, and figuring out what would make the comparison concrete. That conversation took an afternoon.

That evening, I gave the plan to Claude Code and stepped away.

By the next day, most of the transpilations were already in place. A few follow-up prompts, some checking, some editing, and the series had taken shape far faster than either of us expected. What stayed with me was not triumph so much as disorientation: I had assumed this would be a month of labor, and instead it arrived in days.

The entire series — four transpilation experiments across seven components, 9,333 lines of Rust converted to 6,994 lines of C++, 102 behavioral tests, five articles, and an appendix — was conceived, executed, written, and edited in a matter of days.

I still have not quite caught up with that.

A year ago, this project would have required a small team working for months. The transpilation alone — understanding each Rust codebase, designing C++ equivalents, writing build systems, porting tests — would have been a full-time job. Nobody would have done it just to examine a question about language design this directly. The idea would probably have remained a thought experiment, not because it was right or wrong, but because testing it was too expensive.

AI didn't change what was *true*. It changed what was practical to test in a short amount of time.

It moved this experiment from "interesting thought experiment" to "something one person could actually finish." The cost of running a concrete check against a broad intuition dropped dramatically.

This matters beyond this little series. A lot of ideas survive simply because checking them carefully takes too much labor. When that cost drops, more questions become worth testing directly instead of circling around rhetorically.

I don't know what this means for programming language discourse specifically. But I think it means something for how ideas get tested in general. The bottleneck is shifting. It used to be execution — can you build the thing that would let you check your intuition? Now it is more often judgment — is the intuition worth checking in the first place, and what do you do with the answer once you have it?

I had the idea. The tools did much of the mechanical work. But the harder part was still recognizing a question worth asking, and staying honest once the evidence began to answer it.

I am not yet sure what all of this means. I only know that something about the scale of what became possible has quietly changed.

---

*March 2026, Tokyo*
