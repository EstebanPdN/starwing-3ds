#pragma once
#include <openxr/openxr.h>
#include <array>
#include <string>
#include <optional>
namespace starfox::vr {
struct InputApi {
    PFN_xrCreateActionSet create_set{xrCreateActionSet};
    PFN_xrDestroyActionSet destroy_set{xrDestroyActionSet};
    PFN_xrCreateAction create_action{xrCreateAction};
    PFN_xrStringToPath path{xrStringToPath};
    PFN_xrSuggestInteractionProfileBindings suggest{xrSuggestInteractionProfileBindings};
    PFN_xrAttachSessionActionSets attach{xrAttachSessionActionSets};
    PFN_xrSyncActions sync{xrSyncActions};
    PFN_xrGetActionStateBoolean boolean{xrGetActionStateBoolean};
    PFN_xrGetActionStateVector2f vector{xrGetActionStateVector2f};
    PFN_xrCreateActionSpace create_space{xrCreateActionSpace};
    PFN_xrDestroySpace destroy_space{xrDestroySpace};
    PFN_xrLocateSpace locate{xrLocateSpace};
};
struct VrControls {
    XrVector2f steer{};
    bool fire{},bomb{},boost{},brake{},menu{},menu_pressed{},roll_left{},roll_right{};
    bool select{},select_pressed{};
    bool stick_left{},stick_right{},reset_pressed{};
};
class OpenXrInput {
public:
    explicit OpenXrInput(InputApi api={}):api_(api) {}
    ~OpenXrInput();
    OpenXrInput(const OpenXrInput&)=delete;
    OpenXrInput& operator=(const OpenXrInput&)=delete;
    // Attach before session begin. OpenXR permits attachment only once per
    // session; reinitialization requires a fresh caller-owned session.
    bool initialize(XrInstance,XrSession);
    bool poll(bool focused);
    std::array<std::optional<XrPosef>,2> aim_poses(XrSpace base,XrTime time) const noexcept;
    void close() noexcept;
    const VrControls& controls() const noexcept {return controls_;}
    const std::string& status() const noexcept {return status_;}
private:
    InputApi api_;
    XrSession session_{};XrActionSet set_{};std::array<XrAction,13> actions_{};
    std::array<XrSpace,2> aim_spaces_{};
    VrControls controls_{};bool menu_armed_{},select_armed_{};
    bool reset_armed_{};
    std::string status_;
};
}
