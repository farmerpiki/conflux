# conflux.json Design Notes

## Process-lifetime state

`conflux.json` has one permitted process-lifetime singleton: the internal
`CLocaleHolder` used by the slow floating-point range classifier.

The parser normally uses `std::from_chars` for JSON numbers. On current
libstdc++ builds, `from_chars` may report `std::errc::result_out_of_range`
without leaving enough information in the destination `double` to distinguish
finite underflow from overflow-to-infinity. The slow classifier therefore falls
back to `strtod_l` with an explicit C locale for that narrow error path.

The C locale object is intentionally cached for the process lifetime instead of
being created and destroyed per number parse. That keeps locale setup out of the
hot path, avoids repeated libc allocation, and keeps JSON decimal parsing
independent from the process locale. It is never freed because there is no
benefit to doing so during normal shutdown and because freeing it would add
cross-thread lifetime risk for an internal fallback path.

Design constraints:

- no other hidden global or singleton state in `conflux.json`;
- no parser behavior may depend on the process locale;
- all per-document mutable state belongs to `DocumentStorage`, `JsonArena`, or
  caller-provided storage;
- any future process-lifetime cache must be documented here before it is added.

Removal condition: delete `CLocaleHolder`, `strtod_l`, and this exception once
the supported libstdc++/libc++ matrix provides `from_chars` behavior that fully
passes the number overflow/underflow gate without the locale fallback.
