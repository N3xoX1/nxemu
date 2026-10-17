#pragma once
#include "dialog.h"
#include <SDL.h>
#include <array>
#include <functional>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu/ui/input/input_params.h>
#include <vector>

namespace Rml
{
class Element;
} // namespace Rml

class EmulatorSession;
class UiWindow;

// Sets what each player holds and which keys, buttons and sticks of the
// computer stand for the console's. A change applies at once, so that it can
// be tried in the dialog; it is kept when the dialog is saved and undone when
// it is cancelled.
class ControllerDialog final : public Dialog
{
public:
    ControllerDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir);
    ~ControllerDialog();

    // The keyboard goes to the emulated controllers while the dialog is open:
    // to bind its keys and to try the bindings.
    void HandleKey(const SDL_KeyboardEvent & event);
    // Waiting for an input to bind: what the dialog shows changes without an
    // event.
    bool Listening() const { return m_listenSlot >= 0; }
    // The modules are about to be replaced: closes at once, changes undone.
    void Abandon();

private:
    static constexpr int kPlayers = 8;

    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    IOperatingSystem & Os() const;
    // The controller the player's settings act on: for the first player, the
    // console itself when it is played handheld.
    IEmulatedController & Pad(int player) const;
    NpadStyleIndex Type(int player) const;
    void SetType(int player, NpadStyleIndex type);
    void SetConnected(int player, bool connected);
    void Finish(bool save);

    void BuildRows();
    void ReadDevices();
    void FillDevices();
    void FillTypes();
    int MappedDevice(int player) const;
    void ApplyDevice(int player, int device);
    void ShowPlayers();
    void ShowPlayer();
    void ShowBindings();
    void ShowSticks();
    void ShowPicture();
    void ShowState();
    void ShowHint();
    void ApplyChange(const std::string & id);

    InputParams SlotParams(int slot) const;
    std::string SlotText(int slot) const;
    void Bind(int slot, const InputParams & input);
    bool SlotShown(int slot) const;
    void Listen(int slot, bool sequence);
    void StopListening();
    void PollListening();
    bool Accepts(const InputParams & input) const;

    // A button or a stick of the picture and the control it stands for.
    struct Mark
    {
        Rml::Element * element;
        int slot;
    };

    UiWindow & m_window;
    EmulatorSession & m_session;
    int m_player = 0;
    bool m_handheld = false; // the first player plays the console itself
    bool m_changed = false;  // something differs from what is saved
    std::vector<InputParams> m_devices;       // what can be bound; the first is any of them
    std::array<int, kPlayers> m_device{};     // the one each player binds from, in m_devices
    std::array<bool, kPlayers> m_vibration{}; // each player's vibration, and
    std::array<int, kPlayers> m_strength{};   // its strength in percent
    uint64_t m_devicesRead = 0;               // when, in SDL ticks
    std::vector<Rml::Element *> m_rows;       // one per slot, of the open document
    std::vector<Mark> m_marks;                // of the picture shown
    bool m_pictureShown = false;              // the picture is of
    NpadStyleIndex m_pictureType{};           // this type of controller,
    float m_pictureRoom = 0.0f;               // in a room this wide, in dp
    std::array<float, 2> m_stickView{};       // how wide the picture shows each stick's travel, in dp
    std::array<float, 2> m_capTravel{};       // how far the picture moves each stick's cap, in dp
    std::vector<std::string> m_changes;       // ids of the controls changed since the last frame

    int m_listenSlot = -1;        // the slot waiting for an input, or -1
    bool m_listenBegun = false;   // the input module is recording inputs for it
    bool m_listenSequence = false; // the next slot follows when this one is bound
    uint64_t m_listenFrom = 0;    // when recording starts and
    uint64_t m_listenUntil = 0;   // when the wait is given up, in SDL ticks
    std::string m_listenIgnore;   // the input bound last, which its release may repeat
    int m_shownSeconds = -1;
};
