// Shared helpers for decoders that surface Vorbis-comment metadata
// (FLAC, Opus, Ogg Vorbis). Cog's tag-name canonicalisation and the
// base64 encoder used for embedded album art live here so the three
// decoders stay in lockstep.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tuxedo::vorbis_common {

// ASCII lowercasing. Vorbis-comment keys are defined as ASCII.
std::string lowercase(const std::string &s);

// Cog's tag-name canonicalisations. Applied after lowercasing:
//   "lyrics" / "unsynced lyrics" → "unsyncedlyrics"
//   "comments:itunnorm"          → "soundcheck"
// All other keys pass through unchanged.
std::string canonicalise_tag(const std::string &lower_name);

// RFC 4648 base64 with padding. Used for inlining album art bytes
// into the metadata JSON.
std::string base64_encode(const uint8_t *data, size_t len);

// RFC 4648 base64 decoder. Skips whitespace; returns an empty vector
// on malformed input. Used for unwrapping Vorbis-comment
// METADATA_BLOCK_PICTURE payloads.
std::vector<uint8_t> base64_decode(const char *data, size_t len);

// Unpack a FLAC-format PICTURE block (the payload of a Vorbis
// METADATA_BLOCK_PICTURE after base64 decoding) into its MIME type +
// raw image bytes. Returns false if the block is malformed or
// truncated. `mime_out` may be empty on success (some encoders omit).
bool unpack_flac_picture(const uint8_t *data, size_t len,
                         std::string &mime_out, std::vector<uint8_t> &bytes_out);

// Append a tag entry (name=value, raw) into `tags`. Handles splitting
// on '=', lowercasing, canonicalisation, and the multi-value array
// shape. Entries whose name is "metadata_block_picture" or
// "waveformatextensible_channel_mask" are ignored here — callers that
// care about those side-channels must pre-filter before calling.
void accept_tag(nlohmann::json &tags, const char *name, size_t name_len,
                const char *value, size_t value_len);

// Same, but the caller has an "NAME=VALUE" byte range (what libFLAC
// hands back). Splits on the first '=' and dispatches to accept_tag.
void accept_entry(nlohmann::json &tags, const char *entry, size_t length);

} // namespace tuxedo::vorbis_common
