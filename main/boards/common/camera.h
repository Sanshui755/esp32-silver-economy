#ifndef CAMERA_H
#define CAMERA_H

#include <expected>
#include <string>

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;

    // Camera-level mutual exclusion for concurrent vision tasks.
    // TryLock returns true if the lock was acquired; caller must call Unlock().
    // Default: no-op (boards without concurrent camera access are unaffected).
    virtual bool TryLock() { return true; }
    virtual void Unlock() {}
};

#endif  // CAMERA_H
