#pragma once

// СНАЧАЛА подключаем стандартные библиотеки
#include <windows.h>
#include <string>
#include <vector>
#include <DirectXMath.h>

// ПОТОМ определяем и подключаем tinyobjloader
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

using namespace DirectX;

struct SponzaVertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
};

struct SponzaMesh
{
    std::vector<SponzaVertex> vertices;
    std::vector<uint32_t> indices;
    std::string diffuseTextureName;
};

class SponzaModel
{
public:
    std::vector<SponzaMesh> meshes;

    bool LoadFromOBJ(const std::string& filename)
    {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warn, err;

        // Получаем путь к папке с моделью
        std::string basePath = filename.substr(0, filename.find_last_of("\\/") + 1);

        bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err,
            filename.c_str(), basePath.c_str(), true);

        if (!warn.empty()) OutputDebugStringA(warn.c_str());
        if (!err.empty()) OutputDebugStringA(err.c_str());
        if (!ret) return false;

        // Создаем меши (по одному на каждую группу)
        for (const auto& shape : shapes)
        {
            SponzaMesh mesh;

            // Определяем текстуру для этого меша (если есть)
            int materialId = shape.mesh.material_ids[0];
            if (materialId >= 0 && materialId < materials.size())
            {
                mesh.diffuseTextureName = materials[materialId].diffuse_texname;
            }

            // Загружаем вершины
            size_t indexOffset = 0;
            for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
            {
                int fv = shape.mesh.num_face_vertices[f];
                for (size_t v = 0; v < fv; v++)
                {
                    tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];

                    SponzaVertex vertex;

                    // Позиция
                    vertex.Position.x = attrib.vertices[3 * idx.vertex_index + 0];
                    vertex.Position.y = attrib.vertices[3 * idx.vertex_index + 1];
                    vertex.Position.z = attrib.vertices[3 * idx.vertex_index + 2];

                    // Нормаль
                    if (idx.normal_index >= 0)
                    {
                        vertex.Normal.x = attrib.normals[3 * idx.normal_index + 0];
                        vertex.Normal.y = attrib.normals[3 * idx.normal_index + 1];
                        vertex.Normal.z = attrib.normals[3 * idx.normal_index + 2];
                    }

                    // Текстурные координаты
                    if (idx.texcoord_index >= 0)
                    {
                        vertex.TexCoord.x = attrib.texcoords[2 * idx.texcoord_index + 0];
                        vertex.TexCoord.y = 1.0f - attrib.texcoords[2 * idx.texcoord_index + 1];
                    }

                    mesh.vertices.push_back(vertex);
                    mesh.indices.push_back(mesh.indices.size());
                }
                indexOffset += fv;
            }

            if (!mesh.vertices.empty())
            {
                meshes.push_back(mesh);
            }
        }

        return true;
    }
};