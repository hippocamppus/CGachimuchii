#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include "model.h"

Model::Model(const char* filename) : verts_(), uv_(), norms_(), faces_(), faces_uv_(), faces_norm_(), diffusemap_() {
    std::ifstream in;
    in.open(filename, std::ifstream::in);
    if (in.fail()) {
        std::cerr << "ERROR: Cannot open file " << filename << std::endl;
        return;
    }
    std::string line;
    while (!in.eof()) {
        std::getline(in, line);
        std::istringstream iss(line.c_str());
        char trash;
        if (!line.compare(0, 2, "v ")) {
            iss >> trash;
            Vec3f v;
            for (int i = 0; i < 3; i++) iss >> v.raw[i];
            verts_.push_back(v);
        }
        else if (!line.compare(0, 3, "vt ")) {
            iss >> trash >> trash;
            Vec2f uv;
            for (int i = 0; i < 2; i++) iss >> uv.raw[i];
            uv_.push_back(uv);
        }
        else if (!line.compare(0, 3, "vn ")) {
            iss >> trash;
            Vec3f n;
            for (int i = 0; i < 3; i++) iss >> n.raw[i];
            norms_.push_back(n);
        }
        else if (!line.compare(0, 2, "f ")) {
            std::vector<int> f;
            std::vector<int> f_uv;
            std::vector<int> f_norm;
            int v_idx, vt_idx, vn_idx;
            iss >> trash;
            while (iss >> v_idx >> trash >> vt_idx >> trash >> vn_idx) {
                f.push_back(v_idx - 1);
                f_uv.push_back(vt_idx - 1);
                f_norm.push_back(vn_idx - 1);
            }
            faces_.push_back(f);
            faces_uv_.push_back(f_uv);
            faces_norm_.push_back(f_norm);
        }
    }
    in.close();

    std::cerr << "# v# " << verts_.size() << " f# " << faces_.size()
        << " vt# " << uv_.size() << " vn# " << norms_.size() << std::endl;


    if (norms_.empty() && !faces_.empty()) {
        std::cerr << "No normals found in .obj, generating..." << std::endl;
        norms_.resize(verts_.size(), Vec3f(0, 0, 0));
        for (size_t i = 0; i < faces_.size(); i++) {
            Vec3f v0 = verts_[faces_[i][0]];
            Vec3f v1 = verts_[faces_[i][1]];
            Vec3f v2 = verts_[faces_[i][2]];
            Vec3f face_normal = ((v1 - v0) ^ (v2 - v0));
            face_normal.make_unit();
            for (int j = 0; j < 3; j++) {
                norms_[faces_[i][j]] = norms_[faces_[i][j]] + face_normal;
            }
        }
        for (size_t i = 0; i < norms_.size(); i++) {
            norms_[i].make_unit();
        }


        for (size_t i = 0; i < faces_.size(); i++) {
            faces_norm_.push_back(faces_[i]);
        }
        std::cerr << "Generated " << norms_.size() << " normals" << std::endl;
    }

    load_texture(filename, "_diffuse.tga", diffusemap_);
}

Model::~Model() {
}

int Model::nverts() {
    return (int)verts_.size();
}

int Model::nfaces() {
    return (int)faces_.size();
}

std::vector<int> Model::face(int idx) {
    return faces_[idx];
}

std::vector<int> Model::face_uv(int idx) {
    return faces_uv_[idx];
}

std::vector<int> Model::face_norm(int idx) {
    return faces_norm_[idx];
}

Vec3f Model::vert(int i) {
    return verts_[i];
}

Vec2f Model::uv(int i) {
    return uv_[i];
}

Vec3f Model::normal(int i) {
    if (i < 0 || i >= (int)norms_.size()) {
        return Vec3f(0, 0, 1);
    }
    return norms_[i];
}

void Model::load_texture(std::string filename, const char* suffix, TGAImage& img) {
    std::string texfile(filename);
    size_t dot = texfile.find_last_of(".");
    if (dot != std::string::npos) {
        texfile = texfile.substr(0, dot) + std::string(suffix);
        std::cerr << "Loading texture: " << texfile << "\n";
        if (!img.read_tga_file(texfile.c_str())) {
            std::cerr << "WARNING: Failed to load texture " << texfile << std::endl;
        }
        img.flip_vertically();
    }
}

TGAColor Model::diffuse(Vec2f uvf) {
    if (diffusemap_.get_width() == 0 || diffusemap_.get_height() == 0) {
        return TGAColor(255, 0, 255, 255);
    }
    Vec2i uv(uvf.x * diffusemap_.get_width(), uvf.y * diffusemap_.get_height());
    uv.x = std::max(0, std::min(uv.x, diffusemap_.get_width() - 1));
    uv.y = std::max(0, std::min(uv.y, diffusemap_.get_height() - 1));
    return diffusemap_.get(uv.x, uv.y);
}