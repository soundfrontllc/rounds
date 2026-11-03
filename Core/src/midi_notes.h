#pragma once

namespace rounds {

// A note starting or ending `offset` samples into a block (velocity 0 = off).
struct NoteEvent {
    int offset;
    int note;
    int velocity;
};

}  // namespace rounds
