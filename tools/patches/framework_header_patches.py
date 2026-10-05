"""Required header adaptations for the pinned Arduino 2.0.17 framework."""

USB_ORIG = "ESPUSB(size_t event_task_stack_size=2048, uint8_t event_task_priority=5);"
USB_PATCHED = "ESPUSB(size_t event_task_stack_size=16384, uint8_t event_task_priority=5);"
WS_ANCHOR = "HTTPRaw& raw() { return *_currentRaw; }"
WS_ADDITION = (
    WS_ANCHOR + "\n"
    "  bool hasUpload() const { return _currentUpload != nullptr; }\n"
    "  bool hasRaw() const { return _currentRaw != nullptr; }"
)


def patch_usb_header(source):
    if source.count(USB_PATCHED) == 1 and USB_ORIG not in source:
        return source
    if source.count(USB_ORIG) == 1 and USB_PATCHED not in source:
        return source.replace(USB_ORIG, USB_PATCHED)
    raise RuntimeError("USB.h constructor changed: required event-stack patch cannot be verified")


def patch_webserver_header(source):
    if source.count(WS_ADDITION) == 1 and source.count(WS_ANCHOR) == 1:
        return source
    if "hasUpload()" in source or "hasRaw()" in source or source.count(WS_ANCHOR) != 1:
        raise RuntimeError("WebServer.h changed: required upload/raw null guards cannot be verified")
    return source.replace(WS_ANCHOR, WS_ADDITION)
