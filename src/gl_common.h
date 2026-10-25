// 极简 GL 工具：着色器编译/链接
#pragma once

#include <glad/glad.h>

#include <cstdio>
#include <string>

inline GLuint CompileShader(GLenum type, const char* src, const char* debugName)
{
    const GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[4096] = {};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        std::printf("[shader] %s 编译失败:\n%s\n", debugName, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

inline GLuint LinkProgram(GLuint vs, GLuint fs, const char* debugName)
{
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[4096] = {};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        std::printf("[shader] %s 链接失败:\n%s\n", debugName, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

inline GLuint BuildProgram(const char* vsSrc, const char* fsSrc, const char* debugName)
{
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSrc, debugName);
    if (vs == 0) return 0;
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSrc, debugName);
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }
    const GLuint prog = LinkProgram(vs, fs, debugName);
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}
