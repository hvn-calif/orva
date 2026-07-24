# Spec: Zero-Copy Stream Interface for the Avro C++ binding

Status: APPROVED (GO 2026-07-24), implemented

## Purpose & user problem

Consumers decode Avro container files from gRPC streaming responses. The
payload lives in discontiguous buffer chains (absl::Cord), so the decoder
cannot rely on flat contiguous memory or POSIX file descriptors. The binding
must accept an injected stream abstraction that yields memory chunks via a
Next()/BackUp() paradigm, similar to google::protobuf::io::ZeroCopyInputStream.

The Rust push reader (doc/specs/AvroStreamingIO.md) already accepts
arbitrary chunk boundaries via Feed/NextReady/NextValue, so no Rust changes
are needed. What is missing is the C++ surface: the stream interface itself
and a driver that pumps it, so call sites never hand-write the feed loop.

## Success criteria

- User code wraps a google::protobuf::io::ZeroCopyInputStream (or any
  Next/BackUp-shaped source, including an absl::Cord) and reads decoded
  values without ever touching Feed/NextReady/CloseInput.
- No flattening: chunks are fed to the reader exactly as the stream yields
  them. Peak memory stays bounded (one chunk + the reader's compacted
  internal window + one block), independent of file size.
- The binding takes no protobuf dependency.
- Blocking is owned by C++: if the stream's Next() blocks on the network
  (gRPC), the pull call blocks with it. No threads are added on either side
  of the FFI boundary.

## Design

### 1. Stream interface (defined by the binding)

```cpp
namespace security::avro {

// Mirrors the shape of google::protobuf::io::ZeroCopyInputStream so
// existing implementations adapt with a one-liner, without this binding
// depending on protobuf.
class ZeroCopyInputStream {
 public:
  virtual ~ZeroCopyInputStream() = default;
  // Yields the next chunk. Returns false at end of stream. The pointed-to
  // data must stay valid until the next call on the stream.
  virtual bool Next(const void** data, int* size) = 0;
  // Returns the trailing `count` bytes of the last Next() chunk to the
  // stream. Present for interface parity; the driver below never calls it
  // (see "End of file" note).
  virtual void BackUp(int count) = 0;
  virtual int64_t ByteCount() const = 0;
};

// Header-only adapter for anything with the same shape (e.g. the protobuf
// class itself, instantiated in user code where protobuf is available).
template <typename S>
class StreamAdapter final : public ZeroCopyInputStream { ... wraps S* ... };

}  // namespace security::avro
```

### 2. Pull-style reader over a stream (the user-facing type)

```cpp
// Reads one container file from `stream`. The stream is borrowed and must
// outlive the reader. Internally drives the push DataFileReader:
//   while a value is not ready: stream->Next() -> Feed(chunk)
//   stream end -> CloseInput()
class DataFileStreamReader final {
 public:
  // Consumes the header eagerly so schema errors surface at construction.
  static absl::StatusOr<DataFileStreamReader> Create(
      ZeroCopyInputStream* stream);
  static absl::StatusOr<DataFileStreamReader> CreateWithReaderSchema(
      const AvroSchema& reader_schema, ZeroCopyInputStream* stream);

  // True if another value is available; pumps the stream as needed and
  // blocks while the stream's Next() blocks. False at clean end of file.
  absl::StatusOr<bool> HasNext();
  // Next decoded value. kOutOfRange at clean end; kInvalidArgument for
  // fatal framing/decode errors (fused, like the push reader).
  absl::StatusOr<AvroValue> NextValue();

  absl::StatusOr<AvroSchema> WriterSchema() const;
  absl::Status SetMaxBlockSize(uint64_t bytes);
};
```

This is a thin C++ layer (~80 lines) over the existing push reader; the
existing DataFileReader stays public for callers who want to own the loop.

### 3. End of file and BackUp (format constraint)

An Avro Object Container File has no in-band end marker: after any block a
reader cannot distinguish "file finished" from "next block not yet
delivered" except by end of input. Consequently:

- One stream = exactly one container file. End of stream (`Next() -> false`)
  is what triggers CloseInput and defines a clean end.
- If the transport multiplexes other data after the file, the injected
  stream adapter must bound the file (gRPC message framing typically gives
  this for free).
- The driver therefore always consumes whole chunks and never calls
  BackUp(). BackUp stays on the interface for protobuf shape parity and for
  stream implementations that need it internally.

### 4. Known trade-offs (accepted)

- "Zero-copy" refers to the boundary: no flattening of the Cord/stream into
  one contiguous buffer. Inside the reader there is still one bounded copy
  (Feed appends into the compacted parse window) and the usual per-block
  payload slice + decompression. True zero-copy decode (values borrowing
  stream memory) is not something apache-avro supports and is out of scope.
- NextValue/HasNext block while the stream blocks. Confirmed acceptable:
  the gRPC call sites use synchronous reads. Async callers can pump the
  push DataFileReader directly (it never blocks).
- Zero-size chunks from Next() are skipped, but a run of more than 1024
  consecutive ones is treated as a stream protocol violation and fails the
  read, so a stuck stream cannot livelock the pump loop.
- Create parses only up to the header, so SetMaxBlockSize called right
  after Create applies from the first block onward, except for block
  framing that arrived in the same chunks as the header (already parsed
  under the default cap). Strict per-block caps on untrusted input:
  use the push DataFileReader, whose Create parses nothing.

## Decisions from the interview (2026-07-24)

1. Blocking pull API is fine; DataFileStreamReader is the primary surface
   for stream call sites.
2. API shape: separate DataFileStreamReader class borrowing the stream, not
   a factory on DataFileReader.
3. CordInputStream is deferred to a follow-up; until then Cord users
   subclass ZeroCopyInputStream over cord.Chunks() themselves (~30 lines,
   documented in the header comment).

## Out of scope

- CordInputStream adapter (follow-up; see decision 3).
- Write side. DataFileWriter's take_bytes() already drains incrementally
  and maps 1:1 onto a ZeroCopyOutputStream driver; can be a follow-up spec
  if needed.
- Multiple container files per stream without outer bounding (impossible to
  delimit in-band; see section 3).
- Rust changes of any kind.
