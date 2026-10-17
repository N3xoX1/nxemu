#include "controller_picture.h"

namespace
{
constexpr ControllerPart::Kind kButton = ControllerPart::Kind::Button;
constexpr ControllerPart::Kind kStick = ControllerPart::Kind::Stick;
constexpr ControllerPart::Kind kStickView = ControllerPart::Kind::StickView;
constexpr ControllerPart::Kind kStickCap = ControllerPart::Kind::StickCap;

constexpr uint32_t B(NativeButtonValues button)
{
    return static_cast<uint32_t>(button);
}

// Measured on the drawings the pictures are made from.
constexpr ControllerPart kProParts[] = {
    {kButton, B(NativeButtonValues::A), 0.7574f, 0.2618f, 0.0638f, 0.0882f},
    {kButton, B(NativeButtonValues::B), 0.6915f, 0.3529f, 0.0638f, 0.0882f},
    {kButton, B(NativeButtonValues::X), 0.6915f, 0.1706f, 0.0638f, 0.0882f},
    {kButton, B(NativeButtonValues::Y), 0.6255f, 0.2618f, 0.0638f, 0.0882f},
    {kButton, B(NativeButtonValues::L), 0.1696f, 0.0638f, 0.2549f, 0.1009f},
    {kButton, B(NativeButtonValues::R), 0.5755f, 0.0638f, 0.2549f, 0.1009f},
    {kButton, B(NativeButtonValues::ZL), 0.0562f, 0.0806f, 0.0757f, 0.0779f},
    {kButton, B(NativeButtonValues::ZR), 0.8681f, 0.0806f, 0.0757f, 0.0779f},
    {kButton, B(NativeButtonValues::Plus), 0.5872f, 0.1912f, 0.0383f, 0.0529f},
    {kButton, B(NativeButtonValues::Minus), 0.3745f, 0.1912f, 0.0383f, 0.0529f},
    {kButton, B(NativeButtonValues::DLeft), 0.3062f, 0.4438f, 0.0653f, 0.0547f},
    {kButton, B(NativeButtonValues::DUp), 0.3500f, 0.3821f, 0.0396f, 0.0903f},
    {kButton, B(NativeButtonValues::DRight), 0.3689f, 0.4426f, 0.0653f, 0.0547f},
    {kButton, B(NativeButtonValues::DDown), 0.3509f, 0.4688f, 0.0396f, 0.0903f},
    {kButton, B(NativeButtonValues::Home), 0.5404f, 0.2765f, 0.0426f, 0.0588f},
    {kButton, B(NativeButtonValues::Screenshot), 0.4234f, 0.2853f, 0.0298f, 0.0412f},
    {kStick, 0, 0.1957f, 0.2147f, 0.1362f, 0.1882f},
    {kStick, 1, 0.5404f, 0.3765f, 0.1362f, 0.1882f},
    {kStickView, 0, 0.2979f, 0.6471f, 0.1915f, 0.2647f},
    {kStickView, 1, 0.5106f, 0.6471f, 0.1915f, 0.2647f},
    {kStickCap, 0, 0.2096f, 0.2338f, 0.1085f, 0.1500f},
    {kStickCap, 1, 0.5543f, 0.3956f, 0.1085f, 0.1500f},
};

constexpr ControllerPart kDualJoyconsParts[] = {
    {kButton, B(NativeButtonValues::A), 0.6611f, 0.2233f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::B), 0.6185f, 0.2900f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::X), 0.6185f, 0.1567f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::Y), 0.5760f, 0.2233f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::L), 0.0391f, 0.1622f, 0.1499f, 0.0486f},
    {kButton, B(NativeButtonValues::L), 0.2597f, 0.0135f, 0.1541f, 0.1274f},
    {kButton, B(NativeButtonValues::R), 0.8109f, 0.1622f, 0.1499f, 0.0486f},
    {kButton, B(NativeButtonValues::R), 0.5862f, 0.0135f, 0.1541f, 0.1274f},
    {kButton, B(NativeButtonValues::ZL), 0.0422f, 0.0443f, 0.1342f, 0.0891f},
    {kButton, B(NativeButtonValues::ZR), 0.8236f, 0.0443f, 0.1342f, 0.0891f},
    {kButton, B(NativeButtonValues::Plus), 0.5674f, 0.0733f, 0.0319f, 0.0500f},
    {kButton, B(NativeButtonValues::Minus), 0.1596f, 0.2228f, 0.0287f, 0.0150f},
    {kButton, B(NativeButtonValues::Minus), 0.4006f, 0.0933f, 0.0298f, 0.0156f},
    {kButton, B(NativeButtonValues::DLeft), 0.0543f, 0.2228f, 0.0383f, 0.0240f},
    {kButton, B(NativeButtonValues::DLeft), 0.2857f, 0.5100f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::DUp), 0.3283f, 0.4433f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::DRight), 0.1404f, 0.2228f, 0.0383f, 0.0240f},
    {kButton, B(NativeButtonValues::DRight), 0.3709f, 0.5100f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::DDown), 0.3283f, 0.5767f, 0.0426f, 0.0667f},
    {kButton, B(NativeButtonValues::SLLeft), 0.4474f, 0.2200f, 0.0170f, 0.0733f},
    {kButton, B(NativeButtonValues::SRLeft), 0.4474f, 0.5833f, 0.0170f, 0.0733f},
    {kButton, B(NativeButtonValues::Home), 0.5877f, 0.6717f, 0.0404f, 0.0633f},
    {kButton, B(NativeButtonValues::Screenshot), 0.3709f, 0.6800f, 0.0340f, 0.0533f},
    {kButton, B(NativeButtonValues::SLRight), 0.5355f, 0.5833f, 0.0170f, 0.0733f},
    {kButton, B(NativeButtonValues::SRRight), 0.5355f, 0.2200f, 0.0170f, 0.0733f},
    {kStick, 0, 0.3028f, 0.1900f, 0.0936f, 0.1467f},
    {kStick, 1, 0.6036f, 0.4733f, 0.0936f, 0.1467f},
    {kStickView, 0, 0.0426f, 0.6167f, 0.1915f, 0.3000f},
    {kStickView, 1, 0.7766f, 0.6167f, 0.1915f, 0.3000f},
    {kStickCap, 0, 0.2989f, 0.1850f, 0.1011f, 0.1567f},
    {kStickCap, 1, 0.6000f, 0.4683f, 0.1011f, 0.1567f},
};

constexpr ControllerPart kLeftJoyconParts[] = {
    {kButton, B(NativeButtonValues::L), 0.0887f, 0.1733f, 0.1761f, 0.0417f},
    {kButton, B(NativeButtonValues::L), 0.3945f, 0.0716f, 0.1809f, 0.1092f},
    {kButton, B(NativeButtonValues::L), 0.8560f, 0.0763f, 0.0427f, 0.0180f},
    {kButton, B(NativeButtonValues::ZL), 0.0923f, 0.0723f, 0.1576f, 0.0764f},
    {kButton, B(NativeButtonValues::ZL), 0.9192f, 0.0907f, 0.0564f, 0.0568f},
    {kButton, B(NativeButtonValues::Minus), 0.2302f, 0.2253f, 0.0337f, 0.0129f},
    {kButton, B(NativeButtonValues::Minus), 0.5600f, 0.1400f, 0.0350f, 0.0133f},
    {kButton, B(NativeButtonValues::Minus), 0.8335f, 0.1354f, 0.0180f, 0.0137f},
    {kButton, B(NativeButtonValues::DLeft), 0.1065f, 0.2253f, 0.0450f, 0.0206f},
    {kButton, B(NativeButtonValues::DLeft), 0.4250f, 0.4971f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::DUp), 0.4750f, 0.4400f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::DUp), 0.8335f, 0.4466f, 0.0225f, 0.0566f},
    {kButton, B(NativeButtonValues::DRight), 0.2077f, 0.2253f, 0.0450f, 0.0206f},
    {kButton, B(NativeButtonValues::DRight), 0.5250f, 0.4971f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::DRight), 0.8335f, 0.4980f, 0.0225f, 0.0566f},
    {kButton, B(NativeButtonValues::DDown), 0.4750f, 0.5543f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::DDown), 0.8335f, 0.5520f, 0.0225f, 0.0566f},
    {kButton, B(NativeButtonValues::SLLeft), 0.6149f, 0.2486f, 0.0200f, 0.0629f},
    {kButton, B(NativeButtonValues::SLLeft), 0.8758f, 0.2550f, 0.0234f, 0.0617f},
    {kButton, B(NativeButtonValues::SRLeft), 0.6149f, 0.5600f, 0.0200f, 0.0629f},
    {kButton, B(NativeButtonValues::SRLeft), 0.8758f, 0.5661f, 0.0234f, 0.0617f},
    {kButton, B(NativeButtonValues::Screenshot), 0.5250f, 0.6429f, 0.0400f, 0.0457f},
    {kButton, B(NativeButtonValues::Screenshot), 0.8335f, 0.6394f, 0.0135f, 0.0514f},
    {kStick, 0, 0.4450f, 0.2229f, 0.1099f, 0.1257f},
    {kStickView, 0, 0.0753f, 0.5857f, 0.2249f, 0.2571f},
    {kStickCap, 0, 0.4415f, 0.2190f, 0.1170f, 0.1338f},
};

constexpr ControllerPart kRightJoyconParts[] = {
    {kButton, B(NativeButtonValues::A), 0.5125f, 0.2514f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::A), 0.1440f, 0.2383f, 0.0225f, 0.0566f},
    {kButton, B(NativeButtonValues::B), 0.4625f, 0.3086f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::B), 0.1440f, 0.2974f, 0.0225f, 0.0566f},
    {kButton, B(NativeButtonValues::X), 0.4625f, 0.1943f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::X), 0.1440f, 0.1817f, 0.0225f, 0.0566f},
    {kButton, B(NativeButtonValues::Y), 0.4126f, 0.2514f, 0.0500f, 0.0571f},
    {kButton, B(NativeButtonValues::R), 0.7352f, 0.1733f, 0.1761f, 0.0417f},
    {kButton, B(NativeButtonValues::R), 0.4246f, 0.0716f, 0.1809f, 0.1092f},
    {kButton, B(NativeButtonValues::R), 0.1012f, 0.0763f, 0.0427f, 0.0180f},
    {kButton, B(NativeButtonValues::ZR), 0.7500f, 0.0723f, 0.1576f, 0.0764f},
    {kButton, B(NativeButtonValues::ZR), 0.0243f, 0.0907f, 0.0564f, 0.0568f},
    {kButton, B(NativeButtonValues::Plus), 0.4026f, 0.1229f, 0.0375f, 0.0429f},
    {kButton, B(NativeButtonValues::Plus), 0.1305f, 0.1303f, 0.0360f, 0.0265f},
    {kButton, B(NativeButtonValues::Home), 0.4263f, 0.6357f, 0.0475f, 0.0543f},
    {kButton, B(NativeButtonValues::Home), 0.1530f, 0.6343f, 0.0135f, 0.0514f},
    {kButton, B(NativeButtonValues::SLRight), 0.3651f, 0.5600f, 0.0200f, 0.0629f},
    {kButton, B(NativeButtonValues::SLRight), 0.1008f, 0.5661f, 0.0234f, 0.0617f},
    {kButton, B(NativeButtonValues::SRRight), 0.3651f, 0.2486f, 0.0200f, 0.0629f},
    {kButton, B(NativeButtonValues::SRRight), 0.1008f, 0.2550f, 0.0234f, 0.0617f},
    {kStick, 1, 0.4450f, 0.4657f, 0.1099f, 0.1257f},
    {kStickView, 1, 0.6999f, 0.5857f, 0.2249f, 0.2571f},
    {kStickCap, 1, 0.4415f, 0.4611f, 0.1170f, 0.1350f},
};

constexpr ControllerPart kHandheldParts[] = {
    {kButton, B(NativeButtonValues::A), 0.8774f, 0.3312f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::B), 0.8502f, 0.3688f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::X), 0.8502f, 0.2935f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::Y), 0.8230f, 0.3312f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::L), 0.0768f, 0.2256f, 0.0951f, 0.0694f},
    {kButton, B(NativeButtonValues::R), 0.8281f, 0.2256f, 0.0951f, 0.0694f},
    {kButton, B(NativeButtonValues::ZL), 0.0136f, 0.0806f, 0.0757f, 0.0779f},
    {kButton, B(NativeButtonValues::ZR), 0.9106f, 0.0806f, 0.0757f, 0.0779f},
    {kButton, B(NativeButtonValues::Plus), 0.8202f, 0.2603f, 0.0191f, 0.0265f},
    {kButton, B(NativeButtonValues::Minus), 0.1617f, 0.2686f, 0.0170f, 0.0098f},
    {kButton, B(NativeButtonValues::DLeft), 0.0953f, 0.4753f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::DUp), 0.1226f, 0.4376f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::DRight), 0.1498f, 0.4753f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::DDown), 0.1226f, 0.5129f, 0.0272f, 0.0376f},
    {kButton, B(NativeButtonValues::Home), 0.8277f, 0.5588f, 0.0298f, 0.0412f},
    {kButton, B(NativeButtonValues::Screenshot), 0.1447f, 0.5706f, 0.0213f, 0.0294f},
    {kStick, 0, 0.1106f, 0.3147f, 0.0511f, 0.0706f},
    {kStick, 1, 0.8383f, 0.4588f, 0.0511f, 0.0706f},
    {kStickView, 0, 0.3298f, 0.3824f, 0.1277f, 0.1765f},
    {kStickView, 1, 0.5426f, 0.3824f, 0.1277f, 0.1765f},
    {kStickCap, 0, 0.1074f, 0.3103f, 0.0574f, 0.0794f},
    {kStickCap, 1, 0.8351f, 0.4544f, 0.0574f, 0.0794f},
};

constexpr ControllerPart kGameCubeParts[] = {
    {kButton, B(NativeButtonValues::A), 0.6800f, 0.3231f, 0.0840f, 0.1292f},
    {kButton, B(NativeButtonValues::B), 0.6140f, 0.4123f, 0.0520f, 0.0800f},
    {kButton, B(NativeButtonValues::X), 0.7762f, 0.2974f, 0.0495f, 0.1210f},
    {kButton, B(NativeButtonValues::Y), 0.6620f, 0.2270f, 0.0777f, 0.0806f},
    {kButton, B(NativeButtonValues::R), 0.6842f, 0.1341f, 0.1196f, 0.0808f},
    {kButton, B(NativeButtonValues::ZL), 0.2013f, 0.1170f, 0.0994f, 0.0980f},
    {kButton, B(NativeButtonValues::ZR), 0.6994f, 0.1170f, 0.0994f, 0.0980f},
    {kButton, B(NativeButtonValues::DLeft), 0.3178f, 0.6089f, 0.0614f, 0.0572f},
    {kButton, B(NativeButtonValues::DUp), 0.3590f, 0.5443f, 0.0372f, 0.0945f},
    {kButton, B(NativeButtonValues::DRight), 0.3768f, 0.6077f, 0.0614f, 0.0572f},
    {kButton, B(NativeButtonValues::DDown), 0.3598f, 0.6351f, 0.0372f, 0.0945f},
    {kButton, B(NativeButtonValues::Plus), 0.4390f, 0.3072f, 0.1221f, 0.1051f},
    {kStick, 0, 0.2260f, 0.3077f, 0.1040f, 0.1600f},
    {kStick, 1, 0.5920f, 0.5908f, 0.0600f, 0.0923f},
    {kStickView, 0, 0.0140f, 0.0000f, 0.1800f, 0.2769f},
    {kStickView, 1, 0.8060f, 0.0000f, 0.1800f, 0.2769f},
    {kStickCap, 0, 0.2223f, 0.3028f, 0.1117f, 0.1702f},
    {kStickCap, 1, 0.5883f, 0.5843f, 0.0670f, 0.1047f},
};

// A stick's cap and how far it moves, as a fraction of the picture's width.
struct Cap
{
    const char * image = nullptr;
    float travel = 0.0f;
};

template <size_t count>
constexpr ControllerPicture Picture(const char * image, float width, float height, const ControllerPart (&parts)[count], Cap left, Cap right)
{
    return {image, width, height, {left.image, right.image}, {left.travel, right.travel}, parts, count};
}

constexpr ControllerPicture kPro = Picture("images/controllers/pro_controller.png", 940.0f, 680.0f, kProParts,
                                           {"images/controllers/pro_controller_stick0.png", 0.0234f}, {"images/controllers/pro_controller_stick1.png", 0.0234f});
constexpr ControllerPicture kDualJoycons = Picture("images/controllers/dual_joycons.png", 940.0f, 600.0f, kDualJoyconsParts,
                                                   {"images/controllers/dual_joycons_stick0.png", 0.0213f}, {"images/controllers/dual_joycons_stick1.png", 0.0202f});
constexpr ControllerPicture kLeftJoycon = Picture("images/controllers/left_joycon.png", 940.0f, 822.0f, kLeftJoyconParts,
                                                  {"images/controllers/left_joycon_stick0.png", 0.0250f}, {});
constexpr ControllerPicture kRightJoycon = Picture("images/controllers/right_joycon.png", 940.0f, 822.0f, kRightJoyconParts,
                                                   {}, {"images/controllers/right_joycon_stick1.png", 0.0237f});
constexpr ControllerPicture kHandheld = Picture("images/controllers/handheld.png", 940.0f, 680.0f, kHandheldParts,
                                                {"images/controllers/handheld_stick0.png", 0.0106f}, {"images/controllers/handheld_stick1.png", 0.0106f});
constexpr ControllerPicture kGameCube = Picture("images/controllers/gc_controller.png", 940.0f, 611.0f, kGameCubeParts,
                                                {"images/controllers/gc_controller_stick0.png", 0.0200f}, {"images/controllers/gc_controller_stick1.png", 0.0190f});
} // namespace

const ControllerPicture & PictureOf(NpadStyleIndex type)
{
    switch (type)
    {
    case NpadStyleIndex::JoyconDual: return kDualJoycons;
    case NpadStyleIndex::JoyconLeft: return kLeftJoycon;
    case NpadStyleIndex::JoyconRight: return kRightJoycon;
    case NpadStyleIndex::Handheld: return kHandheld;
    case NpadStyleIndex::GameCube: return kGameCube;
    default: return kPro;
    }
}
