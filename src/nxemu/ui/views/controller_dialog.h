#pragma once
#include "dialog.h"
#include <SDL3/SDL.h>
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

class ControllerDialog final : public Dialog
{
public:
    ControllerDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir);
    ~ControllerDialog();

    void HandleKey(const SDL_KeyboardEvent & event);
    bool Listening() const { return m_listenSlot >= 0; }
    void Abandon();

private:
    static constexpr int kPlayers = 8;

    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    IOperatingSystem & Os() const;
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

    struct Mark
    {
        Rml::Element * element;
        int slot;
    };

    UiWindow & m_window;
    EmulatorSession & m_session;
    int m_player = 0;
    bool m_handheld = false;
    bool m_changed = false;
    std::vector<InputParams> m_devices;
    std::array<int, kPlayers> m_device{};
    std::array<bool, kPlayers> m_vibration{};
    std::array<int, kPlayers> m_strength{};
    uint64_t m_devicesRead = 0;
    std::vector<Rml::Element *> m_rows;
    std::vector<Mark> m_marks;
    bool m_pictureShown = false;
    NpadStyleIndex m_pictureType{};
    float m_pictureRoom = 0.0f;
    std::array<float, 2> m_stickView{};
    std::array<float, 2> m_capTravel{};
    std::vector<std::string> m_changes;

    int m_listenSlot = -1;
    bool m_listenBegun = false;
    bool m_listenSequence = false;
    uint64_t m_listenFrom = 0;
    uint64_t m_listenUntil = 0;
    std::string m_listenIgnore;
    int m_shownSeconds = -1;
};
