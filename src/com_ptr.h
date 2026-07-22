// Minimal COM smart pointer.
//
// Deliberately not WRL::ComPtr: WRL's As() and IID_PPV_ARGS both rely on
// __uuidof, which is an MSVC extension GCC does not implement. Passing IIDs
// explicitly keeps the whole project buildable under both MSVC and MinGW.

#pragma once

#include <unknwn.h>
#include <utility>

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr& other) : p_(other.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) { reset(); p_ = other.p_; if (p_) p_->AddRef(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { reset(); p_ = other.p_; other.p_ = nullptr; }
        return *this;
    }

    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }

    T*  get()   const { return p_; }
    T*  operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // For receiving a new reference from an out-parameter.
    T** put() { reset(); return &p_; }
    void** put_void() { reset(); return reinterpret_cast<void**>(&p_); }

    template <typename U>
    HRESULT query(REFIID iid, ComPtr<U>& out) const {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(iid, out.put_void());
    }

private:
    T* p_ = nullptr;
};
