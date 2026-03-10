#include <vector>
#include <cmath>
#include <cstdlib>
#include <limits>
#include "tgaimage.h"
#include "matrix.h"
#include "model.h"
#include "camera.h"
#include "geometry.h"
#include <iostream>  


Model* model = NULL;
const int width = 800;
const int height = 800;
const int depth = 255;

Matrix create_viewport(int x, int y, int w, int h) {
    Matrix m = Matrix::create_identity(4);
    m[0][3] = x + w / 2.f;
    m[1][3] = y + h / 2.f;
    m[2][3] = depth / 2.f;

    m[0][0] = w / 2.f;
    m[1][1] = h / 2.f;
    m[2][2] = depth / 2.f;

    return m;
}

Vec3f barycentric_coords(Vec3f A, Vec3f B, Vec3f C, Vec3f P) {
    Vec3f s[2];
    for (int i = 2; i--; ) {
        s[i][0] = C[i] - A[i];
        s[i][1] = B[i] - A[i];
        s[i][2] = A[i] - P[i];
    }

    Vec3f u = s[0] ^ s[1];

    if (std::abs(u[2]) > 1e-2)
        return Vec3f(1.f - (u.x + u.y) / u.z, u.y / u.z, u.x / u.z);
    return Vec3f(-1, 1, 1);
}

void draw_triangle(Vec3i* vertices, Vec2f* tex_coords, float* depth_buffer, TGAImage& image, float brightness) {
    Vec2i bboxmin(image.get_width() - 1, image.get_height() - 1);
    Vec2i bboxmax(0, 0);
    Vec2i clamp(image.get_width() - 1, image.get_height() - 1);

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 2; j++) {
            bboxmin[j] = std::max(0, std::min(bboxmin[j], vertices[i][j]));
            bboxmax[j] = std::min(clamp[j], std::max(bboxmax[j], vertices[i][j]));
        }
    }

    Vec3f P;
    for (P.x = bboxmin.x; P.x <= bboxmax.x; P.x++) {
        for (P.y = bboxmin.y; P.y <= bboxmax.y; P.y++) {
            Vec3f bc_screen = barycentric_coords(
                Vec3f(vertices[0].x, vertices[0].y, vertices[0].z),
                Vec3f(vertices[1].x, vertices[1].y, vertices[1].z),
                Vec3f(vertices[2].x, vertices[2].y, vertices[2].z),
                P
            );

            if (bc_screen.x < 0 || bc_screen.y < 0 || bc_screen.z < 0)
                continue;

            P.z = 0;
            P.z += vertices[0].z * bc_screen.x;
            P.z += vertices[1].z * bc_screen.y;
            P.z += vertices[2].z * bc_screen.z;

            int idx = (int)P.x + (int)P.y * width;
            if (depth_buffer[idx] < P.z) {
                depth_buffer[idx] = P.z;

                Vec2f uv;
                uv.x = tex_coords[0].x * bc_screen.x + tex_coords[1].x * bc_screen.y + tex_coords[2].x * bc_screen.z;
                uv.y = tex_coords[0].y * bc_screen.x + tex_coords[1].y * bc_screen.y + tex_coords[2].y * bc_screen.z;

                TGAColor color = model->diffuse(uv);

                color.r *= brightness;
                color.g *= brightness;
                color.b *= brightness;

                image.set(P.x, P.y, color);
            }
        }
    }
}


int main() {
    model = new Model("african_head.obj");

    TGAImage image(width, height, TGAImage::RGB);

    float* zbuffer = new float[width * height];
    for (int i = 0; i < width * height; i++) {
        zbuffer[i] = -std::numeric_limits<float>::max();
    }

    Camera camera(
        Vec3f(0, 0, 25),
        Vec3f(0, 0, 0),
        Vec3f(0, 1, 0)
    );

    Matrix View = camera.get_view_matrix();
    Matrix Projection = camera.get_projection_matrix();
    Matrix ViewPort = create_viewport(width / 8, height / 8, width * 3 / 4, height * 3 / 4);

    Vec3f light_dir(0, 0, -1);

    for (int i = 0; i < model->nfaces(); i++) {
        std::vector<int> face = model->face(i);
        std::vector<int> face_uv = model->face_uv(i);

        Vec3i screen_coords[3];
        Vec3f world_coords[3];
        Vec2f uv_coords[3];

        for (int j = 0; j < 3; j++) {
            Vec3f v = model->vert(face[j]);
            world_coords[j] = v;

            Matrix v4 = to_homogeneous(v);
            Matrix clip = multiply_matrices(ViewPort, multiply_matrices(Projection, multiply_matrices(View, v4)));
            Vec3f screenf = from_homogeneous(clip);

            screen_coords[j] = Vec3i(
                (int)(screenf.x + 0.5f),
                (int)(screenf.y + 0.5f),
                (int)(screenf.z + 0.5f)
            );

            uv_coords[j] = model->uv(face_uv[j]);
        }

        Vec3f n = (world_coords[2] - world_coords[0]) ^ (world_coords[1] - world_coords[0]);
        n.make_unit();
        float intensity = n * light_dir;

        if (intensity > 0) {
            draw_triangle(screen_coords, uv_coords, zbuffer, image, intensity);
        }
    }

    image.flip_vertically();
    image.write_tga_file("output.tga");

    delete[] zbuffer;
    delete model;

    return 0;
}