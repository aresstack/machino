// Port: the NNA inference helper as a PROCESS boundary.
//
// Venus (Ingenic's NN runtime) is uClibc; machino is musl. That is not a
// packaging inconvenience but a loader impossibility -- dlopen across libcs
// does not exist. So inference lives in a separate -muclibc helper binary
// (machino-nna), and THIS port is machinod's only view of it: spawn it,
// exchange lines, notice when it died. The same isolation the cellular
// helper bought us for free: a crashing model can never take the video down,
// because it is not in the process that encodes the video.
//
// The protocol on those lines is the core's business (nna_detector.cpp), not
// this port's: the port moves bytes, the detector speaks the language. Frames
// travel out-of-band through a file (tmpfs on the target) whose path is part
// of the spoken protocol -- a few hundred kB of NV12 per inference does not
// belong on a pipe that also carries the control channel.
#pragma once
#include <string>
#include <vector>

namespace machino {

class INnaProcess {
public:
    virtual ~INnaProcess() = default;

    // Start the helper. argv[0] is the binary. False when it cannot even be
    // started (missing binary, fork failure) -- that is "not installed", and
    // the caller reports it as such rather than retrying into the void.
    virtual bool spawn(const std::vector<std::string>& argv) = 0;

    // Is the child still there? Reaps zombies as a side effect.
    virtual bool alive() = 0;

    // One line to the helper's stdin (caller includes the trailing \n).
    virtual bool write_line(const std::string& line) = 0;

    // One \n-terminated line from the helper's stdout, waiting at most
    // timeout_ms. False on timeout or EOF; alive() tells the two apart.
    virtual bool read_line(std::string& out, int timeout_ms) = 0;

    // Stop the child (TERM, then KILL). Idempotent.
    virtual void terminate() = 0;
};

} // namespace machino
