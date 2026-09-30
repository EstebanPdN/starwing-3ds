#include "starfox/vr/openxr_input.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace starfox::vr {
namespace {void check(XrResult r,const char* op) {if(XR_FAILED(r)) throw std::runtime_error(std::string(op)+": "+std::to_string(r));}}
OpenXrInput::~OpenXrInput() {close();}
void OpenXrInput::close() noexcept {
    for(auto& space:aim_spaces_) {if(space) api_.destroy_space(space);space={};}
    if(set_) api_.destroy_set(set_); // Also destroys all child actions.
    session_={};set_={};actions_={};controls_={};menu_armed_=select_armed_=reset_armed_=false;
}
bool OpenXrInput::initialize(XrInstance instance,XrSession session) {
    close();
    if(!instance || !session) {status_="Missing input instance/session";return false;}
    try {
        session_=session;
        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(set.actionSetName,"starfox");std::strcpy(set.localizedActionSetName,"Star Fox");
        check(api_.create_set(instance,&set,&set_),"Create action set");
        const char* names[]{"steer","fire","bomb","boost","brake","menu","roll_left","roll_right","select","aim_left","aim_right","stick_left","stick_right"};
        const char* labels[]{"Steer","Fire","Bomb","Boost","Brake","Menu","Roll Left","Roll Right","Select / Change View","Left pointer","Right pointer","Left stick click","Right stick click"};
        for(unsigned i=0;i<actions_.size();++i) {
            XrActionCreateInfo action{XR_TYPE_ACTION_CREATE_INFO};
            std::strcpy(action.actionName,names[i]);std::strcpy(action.localizedActionName,labels[i]);
            action.actionType=(i==9 || i==10)?XR_ACTION_TYPE_POSE_INPUT:i?XR_ACTION_TYPE_BOOLEAN_INPUT:XR_ACTION_TYPE_VECTOR2F_INPUT;
            check(api_.create_action(set_,&action,&actions_[i]),"Create action");
        }
        const auto path=[&](const char* name) {XrPath value{};check(api_.path(instance,name,&value),"Resolve action path");return value;};
        const auto suggest=[&](const char* profile,std::initializer_list<std::pair<unsigned,const char*>> bindings) {
            std::vector<XrActionSuggestedBinding> values;
            for(const auto& [action,name]:bindings) values.push_back({actions_[action],path(name)});
            XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            info.interactionProfile=path(profile);info.countSuggestedBindings=static_cast<uint32_t>(values.size());info.suggestedBindings=values.data();
            const auto result=api_.suggest(instance,&info);
            // A runtime need not implement every controller profile.
            if(result!=XR_ERROR_PATH_UNSUPPORTED) check(result,"Suggest bindings");
        };
        suggest("/interaction_profiles/khr/simple_controller",{
            {1,"/user/hand/left/input/select/click"},{1,"/user/hand/right/input/select/click"},
            {5,"/user/hand/left/input/menu/click"},{5,"/user/hand/right/input/menu/click"},
            {9,"/user/hand/left/input/aim/pose"},{10,"/user/hand/right/input/aim/pose"}});
        suggest("/interaction_profiles/oculus/touch_controller",{
            {0,"/user/hand/left/input/thumbstick"},{1,"/user/hand/right/input/a/click"},
            {2,"/user/hand/right/input/b/click"},{3,"/user/hand/left/input/x/click"},
            {4,"/user/hand/left/input/y/click"},{5,"/user/hand/right/input/squeeze/value"},
            {6,"/user/hand/left/input/trigger/value"},{7,"/user/hand/right/input/trigger/value"},
            {8,"/user/hand/left/input/squeeze/value"},
            {11,"/user/hand/left/input/thumbstick/click"},{12,"/user/hand/right/input/thumbstick/click"},
            {9,"/user/hand/left/input/aim/pose"},{10,"/user/hand/right/input/aim/pose"}});
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};attach.countActionSets=1;attach.actionSets=&set_;
        // Index has A/B on both hands, not Touch's left X/Y. Its system
        // button belongs to SteamVR. Match Touch: grips are Start/Select,
        // triggers are L/R (and become sandbox grabs while paused).
        suggest("/interaction_profiles/valve/index_controller",{
            {0,"/user/hand/left/input/thumbstick"},{1,"/user/hand/right/input/a/click"},
            {2,"/user/hand/right/input/b/click"},{3,"/user/hand/left/input/a/click"},
            {4,"/user/hand/left/input/b/click"},{5,"/user/hand/right/input/squeeze/value"},
            {6,"/user/hand/left/input/trigger/click"},{7,"/user/hand/right/input/trigger/click"},
            {8,"/user/hand/left/input/squeeze/value"},
            {11,"/user/hand/left/input/thumbstick/click"},{12,"/user/hand/right/input/thumbstick/click"},
            {9,"/user/hand/left/input/aim/pose"},{10,"/user/hand/right/input/aim/pose"}});
        for(unsigned hand=0;hand<2;++hand) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action=actions_[9+hand];info.poseInActionSpace.orientation.w=1;
            check(api_.create_space(session_,&info,&aim_spaces_[hand]),"Create pointer space");
        }
        check(api_.attach(session_,&attach),"Attach action set");status_="VR actions attached";return true;
    } catch(const std::exception& e) {status_=e.what();close();return false;}
}
bool OpenXrInput::poll(bool focused) {
    if(!set_ || !session_) {controls_={};status_="VR input not initialized";return false;}
    if(!focused) {controls_={};menu_armed_=select_armed_=reset_armed_=false;return true;}
    try {
        XrActiveActionSet active{set_,XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&active;
        const auto result=api_.sync(session_,&sync);
        if(result==XR_SESSION_NOT_FOCUSED) {controls_={};menu_armed_=select_armed_=reset_armed_=false;return true;}
        check(result,"Sync actions");
        VrControls next;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};get.action=actions_[0];
        XrActionStateVector2f vector{XR_TYPE_ACTION_STATE_VECTOR2F};
        check(api_.vector(session_,&get,&vector),"Read steering");
        if(vector.isActive && std::isfinite(vector.currentState.x) && std::isfinite(vector.currentState.y)) {
            float x=std::clamp(vector.currentState.x,-1.F,1.F),y=std::clamp(vector.currentState.y,-1.F,1.F);
            const float radius=std::hypot(x,y);
            if(radius>.15F) {
                const float scale=(std::min(radius,1.F)-.15F)/(.85F*radius);
                next.steer={x*scale,y*scale};
            }
        }
        bool* buttons[]{&next.fire,&next.bomb,&next.boost,&next.brake,&next.menu,&next.roll_left,&next.roll_right,&next.select};
        bool menu_active=false,select_active=false;
        for(unsigned i=1;i<9;++i) {
            get.action=actions_[i];XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            check(api_.boolean(session_,&get,&state),"Read button");
            *buttons[i-1]=state.isActive && state.currentState;
            if(i==5) menu_active=state.isActive;
            if(i==8) select_active=state.isActive;
        }
        next.menu_pressed=menu_active && menu_armed_ && next.menu && !controls_.menu;
        bool sticks_active=true;
        for(unsigned i=11;i<13;++i) {
            get.action=actions_[i];XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            check(api_.boolean(session_,&get,&state),"Read stick click");
            sticks_active &= bool(state.isActive);
            (i==11?next.stick_left:next.stick_right)=state.isActive && state.currentState;
        }
        const bool triggers=next.roll_left && next.roll_right;
        next.reset_pressed=reset_armed_ && triggers && sticks_active && next.stick_left && next.stick_right;
        if(!triggers || !sticks_active || next.reset_pressed) reset_armed_=false;
        else if(!next.stick_left && !next.stick_right) reset_armed_=true;
        if(!menu_active) menu_armed_=false;
        else if(!next.menu) menu_armed_=true;
        next.select_pressed=select_active && select_armed_ && next.select && !controls_.select;
        if(!select_active) select_armed_=false;
        else if(!next.select) select_armed_=true;
        controls_=next;status_="VR actions synchronized";return true;
    } catch(const std::exception& e) {controls_={};menu_armed_=select_armed_=reset_armed_=false;status_=e.what();return false;}
}
std::array<std::optional<XrPosef>,2> OpenXrInput::aim_poses(XrSpace base,XrTime time) const noexcept {
    std::array<std::optional<XrPosef>,2> result;
    if(!base || time<=0) return result;
    constexpr auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT
        |XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    for(unsigned hand=0;hand<2;++hand) if(aim_spaces_[hand]) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if(XR_SUCCEEDED(api_.locate(aim_spaces_[hand],base,time,&location))
            && (location.locationFlags&valid)==valid) result[hand]=location.pose;
    }
    return result;
}
}
