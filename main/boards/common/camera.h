#ifndef CAMERA_H
#define CAMERA_H

#include <cstdio>
#include <string>
#include <stdexcept>

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::string Explain(const std::string& question) = 0;

    // Stream an already-encoded image file (e.g. read from an SD card) to the
    // same explain endpoint Explain() posts a live capture to. Reads file in
    // small chunks rather than buffering it whole, so this works regardless
    // of file size. Caller retains ownership of file (opens and closes it).
    // content_type is sent as-is, e.g. "image/jpeg" or "image/png".
    // Optional: default throws, boards without an explain-capable camera
    // implementation don't need to support this.
    virtual std::string ExplainFile(FILE* file, size_t file_size, const std::string& content_type,
                                     const std::string& question) {
        throw std::runtime_error("This camera does not support explaining an image file");
    }
};

#endif  // CAMERA_H
