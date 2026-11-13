#version 330 core

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;   // transpose(inverse(mat3(uModel)))

// ---------------------------------------------------------------------------
// Outputs to fragment shader
// ---------------------------------------------------------------------------
out vec3 vFragPos;   // world-space fragment position
out vec3 vNormal;    // world-space normal (pre-normalised; frag shader renormalises)

// ---------------------------------------------------------------------------
void main()
{
    vec4 worldPos = uModel * vec4(aPosition, 1.0);
    vFragPos      = vec3(worldPos);
    vNormal       = normalize(uNormalMatrix * aNormal);
    gl_Position   = uProjection * uView * worldPos;
}
