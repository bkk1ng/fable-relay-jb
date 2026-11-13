#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix; // transpose(inverse(mat3(uModel)))

out vec3 vFragPos;   // world-space position
out vec3 vNormal;    // world-space normal (not yet normalised)
out vec2 vTexCoord;

void main()
{
    vec4 worldPos   = uModel * vec4(aPosition, 1.0);
    vFragPos        = worldPos.xyz;
    vNormal         = uNormalMatrix * aNormal;
    vTexCoord       = aTexCoord;
    gl_Position     = uProjection * uView * worldPos;
}
