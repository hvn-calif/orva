# Apache Avro 0.21 decoder patch

This branch targets Apache Avro 0.21 and calls the reusable decoder API added
by `apache-avro-0.21-read-into.patch`. That API is not in the published 0.21
crate, so the branch is expected to be paired with a locally patched Avro
checkout.

Instructions for the receiving Gemini agent:

1. Check out Apache Avro commit
   `04707999f75278fdea927ee8d2a59de41d8f22a7`, the 0.21.0 release.
2. Apply `patches/apache-avro-0.21-read-into.patch` from this repository.
3. In the receiving workspace only, redirect Orva's `apache-avro` dependency
   to the patched crate, for example with a path dependency or an appropriate
   Cargo source override.
4. Do not commit the receiving machine's checkout path to this repository.
5. Build and run the Rust and C++ tests before using the integration.

The patch is a normal `git format-patch` artifact and may be applied with
`git am` or `git apply`.
