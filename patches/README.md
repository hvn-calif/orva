# Apache Avro 0.22 decoder patch

This branch targets Apache Avro 0.22 and calls the reusable decoder API added
by `apache-avro-0.22-read-into.patch`. The pinned upstream revision does not
contain that API, so the branch is expected to be paired with a locally
patched Avro checkout until the change is upstream.

Instructions for the receiving Gemini agent:

1. Check out Apache Avro commit
   `006ac8976f52af356beb5042788370f645f6da02`.
2. Apply `patches/apache-avro-0.22-read-into.patch` from this repository.
3. In the receiving workspace only, redirect Orva's `apache-avro` dependency
   to the patched crate, for example with a path dependency or an appropriate
   Cargo source override.
4. Do not commit the receiving machine's checkout path to this repository.
5. Build and run the Rust and C++ tests before using the integration.

The patch is a normal `git format-patch` artifact and may be applied with
`git am` or `git apply`.
