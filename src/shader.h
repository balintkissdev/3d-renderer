#pragma once

#include "glad/gl.h"
#include "glm/vec3.hpp"
#include "glm/mat4x3.hpp"
#include "glm/mat4x4.hpp"

#include <memory>
#include <string>
#include <vector>

class Shader
{
public:
    static std::unique_ptr<Shader> createFromFile(const char *vertexShaderPath, const char *fragmentShaderPath);

    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void use();

    template <typename T>
    void setUniform(const char *name, const T &v);

    void updateSubroutines(const GLenum shaderType, const std::vector<std::string> &names);

private:
    static std::string readFile(const char *shaderPath);
    static bool checkCompileErrors(GLuint shaderID, const GLenum shaderType);
    static bool checkLinkerErrors(GLuint shaderID);

    Shader();

    GLuint shaderProgram_;
    std::vector<GLuint> subroutineIndices_;
};

template <>
void Shader::setUniform(const char *name, const int &v);
template <>
void Shader::setUniform(const char *name, const float (&v)[3]);
template <>
void Shader::setUniform(const char *name, const glm::vec3 &v);
template <>
void Shader::setUniform(const char *name, const glm::mat3 &v);
template <>
void Shader::setUniform(const char *name, const glm::mat4 &v);
