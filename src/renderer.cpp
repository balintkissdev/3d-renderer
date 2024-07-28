#include "renderer.h"

#include "camera.h"
#include "drawproperties.h"
#include "model.h"
#include "shader.h"
#include "skybox.h"
#include "utils.h"

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#ifdef __EMSCRIPTEN__
#include "glad/gles2.h"

#include <GLFW/glfw3.h>  // Use GLFW port from Emscripten
#else
#include "GLFW/glfw3.h"
#include "glad/gl.h"
#endif

Renderer::Renderer(const DrawProperties& drawProps, const Camera& camera)
    : window_{nullptr}
    , drawProps_(drawProps)
    , camera_(camera)
{
}

bool Renderer::init(GLFWwindow* window)
{
    // Set OpenGL function addresses
#ifdef __EMSCRIPTEN__
    if (!gladLoadGLES2(glfwGetProcAddress))
#else
    if (!gladLoadGL(glfwGetProcAddress))
#endif
    {
        utils::showErrorMessage("unable to load OpenGL extensions");
        return false;
    }

    // Load shaders
#ifdef __EMSCRIPTEN__
    const char* modelVertexShaderPath = "assets/shaders/model_gles3.vert.glsl";
    const char* modelFragmentShaderPath
        = "assets/shaders/model_gles3.frag.glsl";
    const char* skyboxVertexShaderPath
        = "assets/shaders/skybox_gles3.vert.glsl";
    const char* skyboxFragmentShaderPath
        = "assets/shaders/skybox_gles3.frag.glsl";
#else
    const char* modelVertexShaderPath = "assets/shaders/model_gl4.vert.glsl";
    const char* modelFragmentShaderPath = "assets/shaders/model_gl4.frag.glsl";
    const char* skyboxVertexShaderPath = "assets/shaders/skybox_gl4.vert.glsl";
    const char* skyboxFragmentShaderPath
        = "assets/shaders/skybox_gl4.frag.glsl";
#endif
    std::optional<Shader> modelShader
        = Shader::createFromFile(modelVertexShaderPath,
                                 modelFragmentShaderPath);
    if (!modelShader)
    {
        return false;
    }

    std::optional<Shader> skyboxShader
        = Shader::createFromFile(skyboxVertexShaderPath,
                                 skyboxFragmentShaderPath);
    if (!skyboxShader)
    {
        return false;
    }
    shaders_.reserve(2);
    shaders_.emplace_back(std::move(modelShader.value()));
    shaders_.emplace_back(std::move(skyboxShader.value()));

    // Customize OpenGL capabilities
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    window_ = window;

    return true;
}

void Renderer::prepareDraw()
{
    // Viewport setup
    int frameBufferWidth, frameBufferHeight;
    glfwGetFramebufferSize(window_, &frameBufferWidth, &frameBufferHeight);
    glViewport(0, 0, frameBufferWidth, frameBufferHeight);
    projection_ = glm::perspective(glm::radians(drawProps_.fov),
                                   static_cast<float>(frameBufferWidth)
                                       / static_cast<float>(frameBufferHeight),
                                   0.1F,
                                   100.0F);

    // Clear screen
    glClearColor(drawProps_.backgroundColor[0],
                 drawProps_.backgroundColor[1],
                 drawProps_.backgroundColor[2],
                 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::drawModel(const Model& model)
{
    // Set model draw shader
    auto& shader
        = shaders_[static_cast<std::uint8_t>(ShaderInstance::ModelShader)];
    shader.use();
    // Set vertex input
    glBindVertexArray(model.vertexArray);

    // Model transform
    // Avoid Gimbal-lock by converting Euler angles to quaternions
    const glm::quat quatX
        = glm::angleAxis(glm::radians(drawProps_.modelRotation[0]),
                         glm::vec3(1.0F, 0.0F, 0.0F));
    const glm::quat quatY
        = glm::angleAxis(glm::radians(drawProps_.modelRotation[1]),
                         glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::quat quatZ
        = glm::angleAxis(glm::radians(drawProps_.modelRotation[2]),
                         glm::vec3(0.0F, 0.0F, 1.0F));
    const glm::quat quat = quatZ * quatY * quatX;
    const auto modelMatrix = glm::mat4_cast(quat);

    // Concat matrix transformations on CPU to avoid unnecessary multiplications
    // in GLSL. Results would be the same for all vertices.
    const glm::mat4 view = camera_.calculateViewMatrix();
    const glm::mat4 mvp = projection_ * view * modelMatrix;
    const glm::mat3 normalMatrix
        = glm::mat3(glm::transpose(glm::inverse(modelMatrix)));

    // Transfer uniforms
    shader.setUniform("u_model", modelMatrix);
    shader.setUniform("u_mvp", mvp);
    shader.setUniform("u_normalMatrix", normalMatrix);
    shader.setUniform("u_color", drawProps_.modelColor);
    shader.setUniform("u_light.direction", drawProps_.lightDirection);
    shader.setUniform("u_viewPos", camera_.position());
#ifdef __EMSCRIPTEN__
    // GLSL subroutines are not supported in OpenGL ES 3.0
    shader.setUniform("u_adsProps.diffuseEnabled", drawProps_.diffuseEnabled);
    shader.setUniform("u_adsProps.specularEnabled", drawProps_.specularEnabled);
#else
    shader.updateSubroutines(
        GL_FRAGMENT_SHADER,
        {drawProps_.diffuseEnabled ? "DiffuseEnabled" : "Disabled",
         drawProps_.specularEnabled ? "SpecularEnabled" : "Disabled"});
    // glPolygonMode is not supported in OpenGL ES 3.0
    glPolygonMode(GL_FRONT_AND_BACK,
                  drawProps_.wireframeModeEnabled ? GL_LINE : GL_FILL);
#endif

    // Issue draw call
    glDrawElements(GL_TRIANGLES,
                   static_cast<GLsizei>(model.indices.size()),
                   GL_UNSIGNED_INT,
                   nullptr);

    // Reset state
#ifndef __EMSCRIPTEN__
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
    glBindVertexArray(0);
}

void Renderer::drawSkybox(const Skybox& skybox)
{
    // Skybox needs to be drawn at the end of the rendering pipeline for
    // efficiency, not the other way around before objects (like in Painter's
    // Algorithm).
    //
    // Allow skybox pixel depths to pass depth test even when depth buffer is
    // filled with maximum 1.0 depth values. Everything drawn before skybox
    // will be displayed in front of skybox.
    glDepthFunc(GL_LEQUAL);
    // Set skybox shader
    auto& shader
        = shaders_[static_cast<std::uint8_t>(ShaderInstance::SkyboxShader)];
    shader.use();
    glBindVertexArray(skybox.vertexArray);

    // Set skybox texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skybox.textureID);

    // Remove camera position transformations but keep rotation by recreating
    // view matrix, then converting to mat3 and back. If you don't do this,
    // skybox will be shown as a shrinked down cube around model.
    const glm::mat4 normalizedView
        = glm::mat4(glm::mat3(camera_.calculateViewMatrix()));
    // Concat matrix transformations on CPU to avoid unnecessary
    // multiplications
    // in GLSL. Results would be the same for all vertices.
    const glm::mat4 projectionView = projection_ * normalizedView;

    // Transfer uniforms
    shader.setUniform("u_projectionView", projectionView);
    constexpr int textureUnit = 0;
    shader.setUniform("u_skyboxTexture", textureUnit);

    // Issue draw call
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr);

    // Reset state
    glBindVertexArray(0);
    glDepthFunc(GL_LESS);  // Reset depth testing to default
}
