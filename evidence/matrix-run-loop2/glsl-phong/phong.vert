#version 330 core

// ── vertex attributes ─────────────────────────────────────────────────────────
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

// ── matrices ──────────────────────────────────────────────────────────────────
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
// transpose(inverse(mat3(uModel))) — upload this from the CPU; computing the
// inverse per-vertex on the GPU is unnecessary and expensive.
uniform mat3 uNormalMatrix;

// ── outputs to fragment stage ─────────────────────────────────────────────────
out vec3 vFragPos;   // world-space fragment position
out vec3 vNormal;    // world-space normal (unnormalised; normalise in frag)
out vec2 vTexCoord;

void main()
{
    vec4 worldPos = uModel * vec4(aPosition, 1.0);

    vFragPos  = worldPos.xyz;
    vNormal   = uNormalMatrix * aNormal;
    vTexCoord = aTexCoord;

    gl_Position = uProjection * uView * worldPos;
}
