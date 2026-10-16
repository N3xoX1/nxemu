#version 450

layout(push_constant) uniform Constants
{
    layout(offset = 16) mat4 colorMatrix;
} constants;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 finalColor;

void main()
{
    // Only the colour channels are transformed, which lets the matrix apply
    // directly in premultiplied space: the constant term scales with alpha.
    vec4 texColor = texture(tex, fragTexCoord);
    finalColor = vec4(vec3(constants.colorMatrix * texColor), texColor.a);
}
