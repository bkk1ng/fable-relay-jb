#version 330 core

// ── inputs from vertex stage ───────────────────────────────────────────────────
in vec3 vFragPos;   // world-space fragment position
in vec3 vNormal;    // world-space normal (unnormalised)
in vec2 vTexCoord;

// ── output ────────────────────────────────────────────────────────────────────
out vec4 fragColor;

// ── camera ────────────────────────────────────────────────────────────────────
uniform vec3 uViewPos;          // world-space camera position

// ── material ──────────────────────────────────────────────────────────────────
uniform vec3  uAmbientColor;    // base ambient reflectance
uniform vec3  uDiffuseColor;    // diffuse reflectance (or sample a texture here)
uniform vec3  uSpecularColor;   // specular reflectance
uniform float uShininess;       // Phong specular exponent

// ── directional light ─────────────────────────────────────────────────────────
uniform vec3  uDirLightDir;     // direction *toward* the light (normalise on CPU)
uniform vec3  uDirLightAmbient;
uniform vec3  uDirLightDiffuse;
uniform vec3  uDirLightSpecular;

// ── point light ───────────────────────────────────────────────────────────────
uniform vec3  uPointLightPos;
uniform vec3  uPointLightAmbient;
uniform vec3  uPointLightDiffuse;
uniform vec3  uPointLightSpecular;
// attenuation: 1 / (c + l*d + q*d^2)
uniform float uPointLightConstant;   // typically 1.0
uniform float uPointLightLinear;     // e.g. 0.09
uniform float uPointLightQuadratic;  // e.g. 0.032

// ── gamma ─────────────────────────────────────────────────────────────────────
uniform float uGamma;           // typically 2.2

// ─────────────────────────────────────────────────────────────────────────────
// Blinn-Phong contribution for a single light.
// All vectors must be normalised before passing in.
// ─────────────────────────────────────────────────────────────────────────────
vec3 phongContrib(
    vec3 ambient,
    vec3 diffuse,
    vec3 specular,
    vec3 lightDir,   // unit vector from fragment toward light
    vec3 normal,     // unit surface normal
    vec3 viewDir,    // unit vector from fragment toward camera
    float attenuation
) {
    // ambient term
    vec3 ambientTerm = ambient * uAmbientColor;

    // diffuse term — clamp avoids negative contribution from behind
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuseTerm = diffuse * (diff * uDiffuseColor);

    // specular term — Blinn-Phong half-vector for a smoother highlight
    vec3  halfDir      = normalize(lightDir + viewDir);
    float spec         = pow(max(dot(normal, halfDir), 0.0), uShininess);
    vec3  specularTerm = specular * (spec * uSpecularColor);

    return attenuation * (ambientTerm + diffuseTerm + specularTerm);
}

void main()
{
    vec3 normal  = normalize(vNormal);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    // ── directional light contribution ────────────────────────────────────────
    // Direction is uniform (no distance attenuation).
    vec3 dirDir = normalize(uDirLightDir);
    vec3 colorDir = phongContrib(
        uDirLightAmbient,
        uDirLightDiffuse,
        uDirLightSpecular,
        dirDir,
        normal,
        viewDir,
        1.0            // no attenuation for directional lights
    );

    // ── point light contribution ───────────────────────────────────────────────
    vec3  pointDir      = normalize(uPointLightPos - vFragPos);
    float pointDist     = length(uPointLightPos - vFragPos);
    float pointAtten    = 1.0 / (
        uPointLightConstant +
        uPointLightLinear    * pointDist +
        uPointLightQuadratic * pointDist * pointDist
    );

    vec3 colorPoint = phongContrib(
        uPointLightAmbient,
        uPointLightDiffuse,
        uPointLightSpecular,
        pointDir,
        normal,
        viewDir,
        pointAtten
    );

    // ── combine and gamma-correct ─────────────────────────────────────────────
    // Lighting is computed in linear space; apply gamma at the very end so
    // monitors (which apply the inverse curve) reproduce the intended values.
    vec3 linearColor = colorDir + colorPoint;
    vec3 gammaColor  = pow(linearColor, vec3(1.0 / uGamma));

    fragColor = vec4(gammaColor, 1.0);
}
