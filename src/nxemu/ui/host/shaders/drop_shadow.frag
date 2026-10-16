#version 450

layout(push_constant) uniform Constants
{
    layout(offset = 16) vec2 texCoordMin;
    vec2 texCoordMax;
    vec4 color;
} constants;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 finalColor;

void main()
{
    vec2 inRegion = step(constants.texCoordMin, fragTexCoord) * step(fragTexCoord, constants.texCoordMax);
    finalColor = texture(tex, fragTexCoord).a * inRegion.x * inRegion.y * constants.color;
}
