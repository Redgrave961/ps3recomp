#include "vk_trophy_toast.h"

#include "../rsx_spirv.h"
#include "../../network/sceNpTrophy.h"
#include "../../system/cellSysutil.h"
#include "../../codec/stb_image.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../../third_party/stb/stb_truetype.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vk {

namespace {

uint64_t toast_now_ns()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* toast_heading()
{
    switch (ps3_sysutil_language()) {
    case 0:  return "Trophy unlocked";
    case 2:  return "Vous avez gagn\xC3\xA9 un troph\xC3\xA9" "e.";
    case 3:
    case 20:
    case 21: return "Has ganado un trofeo.";
    case 4:  return "Du hast eine Troph\xC3\xA4" "e verdient.";
    case 5:  return "Hai guadagnato un trofeo.";
    case 7:
    case 17: return "Voc\xC3\xAA ganhou um trof\xC3\xA9u.";
    default: return "You have earned a trophy.";
    }
}

uint32_t utf8_next(const char*& p)
{
    const unsigned char c = (unsigned char)*p;
    if (!c) return 0;
    if (c < 0x80) { p++; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) {
        uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
        p += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
        uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) | ((unsigned char)p[2] & 0x3F);
        p += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        uint32_t cp = ((uint32_t)(c & 0x07) << 18) | (((unsigned char)p[1] & 0x3F) << 12) |
                      (((unsigned char)p[2] & 0x3F) << 6) | ((unsigned char)p[3] & 0x3F);
        p += 4;
        return cp;
    }
    p++;
    return '?';
}

inline uint8_t clamp_u8(float v)
{
    return v <= 0.f ? 0 : (v >= 255.f ? 255 : (uint8_t)(v + 0.5f));
}

void blend_px(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, float a)
{
    if (a <= 0.001f) return;
    if (a > 1.f) a = 1.f;
    const float da = dst[3] / 255.f;
    const float oa = a + da * (1.f - a);
    if (oa <= 1e-5f) return;
    const float inv = 1.f / oa;
    dst[0] = clamp_u8((r * a + dst[0] * da * (1.f - a)) * inv);
    dst[1] = clamp_u8((g * a + dst[1] * da * (1.f - a)) * inv);
    dst[2] = clamp_u8((b * a + dst[2] * da * (1.f - a)) * inv);
    dst[3] = clamp_u8(oa * 255.f);
}

float sd_round_rect(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    const float qx = std::fabs(px - cx) - (hx - r);
    const float qy = std::fabs(py - cy) - (hy - r);
    const float ox = std::max(qx, 0.f);
    const float oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

void draw_trophy_cup(std::vector<uint8_t>& buf, int bw, int bh, int ox, int oy, int sz, char grade)
{
    uint8_t cr = 205, cg = 136, cb = 76;
    uint8_t hr = 240, hg = 190, hb = 135;
    if (grade == 'S') { cr = 198; cg = 206; cb = 218; hr = 246; hg = 250; hb = 255; }
    else if (grade == 'G') { cr = 236; cg = 194; cb = 68; hr = 255; hg = 242; hb = 165; }
    else if (grade == 'P') { cr = 182; cg = 214; cb = 248; hr = 248; hg = 252; hb = 255; }

    const float inv = 1.f / (float)sz;
    for (int py = 0; py < sz; py++) {
        const int dy = oy + py;
        if (dy < 0 || dy >= bh) continue;
        const float v = (py + 0.5f) * inv;
        for (int px = 0; px < sz; px++) {
            const int dx = ox + px;
            if (dx < 0 || dx >= bw) continue;
            const float u = (px + 0.5f) * inv;
            const float ax = std::fabs(u - 0.5f);

            float cov = 0.f;
            if (v >= 0.08f && v <= 0.60f) {
                const float t = (v - 0.08f) / 0.52f;
                const float hw = 0.30f - 0.16f * t * t;
                const float edge = (hw - ax) * sz;
                cov = std::max(cov, std::clamp(edge + 0.5f, 0.f, 1.f));
            }
            if (v >= 0.14f && v <= 0.44f) {
                const float hy = (v - 0.28f) / 0.14f;
                const float hx = (ax - 0.31f) / 0.13f;
                const float d = std::fabs(std::sqrt(hx * hx + hy * hy) - 0.85f);
                const float hcov = std::clamp((0.36f - d) * 3.5f, 0.f, 1.f);
                if (ax > 0.24f) cov = std::max(cov, hcov);
            }
            if (v >= 0.56f && v <= 0.80f) {
                const float edge = (0.075f - ax) * sz;
                cov = std::max(cov, std::clamp(edge + 0.5f, 0.f, 1.f));
            }
            if (v >= 0.77f && v <= 0.93f) {
                const float t = (v - 0.77f) / 0.16f;
                const float hw = 0.18f + 0.10f * t;
                const float edge = (hw - ax) * sz;
                cov = std::max(cov, std::clamp(edge + 0.5f, 0.f, 1.f));
            }
            if (cov <= 0.01f) continue;

            const float shine = std::clamp(1.f - ((u - 0.36f) * (u - 0.36f) * 8.f + (v - 0.26f) * (v - 0.26f) * 3.5f), 0.f, 1.f);
            const float shade = 0.82f + 0.18f * (1.f - v);
            const uint8_t r = clamp_u8(cr * shade + (hr - cr * shade) * shine * 0.75f);
            const uint8_t g = clamp_u8(cg * shade + (hg - cg * shade) * shine * 0.75f);
            const uint8_t b = clamp_u8(cb * shade + (hb - cb * shade) * shine * 0.75f);
            blend_px(&buf[((size_t)dy * bw + dx) * 4], r, g, b, cov);
        }
    }
}

float measure_text(const stbtt_fontinfo* font, float scale, const char* s)
{
    if (!font) return (float)std::strlen(s) * scale * 0.55f;
    float w = 0.f;
    uint32_t prev = 0;
    while (*s) {
        const uint32_t cp = utf8_next(s);
        if (!cp) break;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(font, (int)cp, &adv, &lsb);
        if (prev) w += stbtt_GetCodepointKernAdvance(font, (int)prev, (int)cp) * scale;
        w += adv * scale;
        prev = cp;
    }
    return w;
}

void draw_text(std::vector<uint8_t>& buf, int bw, int bh, const stbtt_fontinfo* font, float scale, int ascent_px,
               float x, float y, const char* s, uint8_t r, uint8_t g, uint8_t b, float max_w)
{
    if (!font) return;
    std::string clipped(s);
    if (max_w > 0.f && measure_text(font, scale, clipped.c_str()) > max_w) {
        const float dots = measure_text(font, scale, "...");
        while (!clipped.empty() && measure_text(font, scale, clipped.c_str()) + dots > max_w) {
            clipped.pop_back();
            while (!clipped.empty() && (((unsigned char)clipped.back() & 0xC0) == 0x80)) clipped.pop_back();
        }
        clipped += "...";
    }
    const char* p = clipped.c_str();
    float cur_x = x;
    uint32_t prev = 0;
    while (*p) {
        const uint32_t cp = utf8_next(p);
        if (!cp) break;
        if (prev) cur_x += stbtt_GetCodepointKernAdvance(font, (int)prev, (int)cp) * scale;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(font, (int)cp, &adv, &lsb);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(font, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<uint8_t> glyph((size_t)gw * gh);
            stbtt_MakeCodepointBitmap(font, glyph.data(), gw, gh, gw, scale, scale, (int)cp);
            const int gx = (int)std::floor(cur_x) + x0;
            const int gy = (int)std::floor(y) + ascent_px + y0;
            for (int row = 0; row < gh; row++) {
                const int dy = gy + row + 1;
                if (dy < 0 || dy >= bh) continue;
                for (int col = 0; col < gw; col++) {
                    const int dx = gx + col + 1;
                    if (dx < 0 || dx >= bw) continue;
                    const float a = glyph[(size_t)row * gw + col] / 255.f;
                    if (a > 0.02f) blend_px(&buf[((size_t)dy * bw + dx) * 4], 0, 0, 0, a * 0.55f);
                }
            }
            for (int row = 0; row < gh; row++) {
                const int dy = gy + row;
                if (dy < 0 || dy >= bh) continue;
                for (int col = 0; col < gw; col++) {
                    const int dx = gx + col;
                    if (dx < 0 || dx >= bw) continue;
                    const float a = glyph[(size_t)row * gw + col] / 255.f;
                    if (a > 0.01f) blend_px(&buf[((size_t)dy * bw + dx) * 4], r, g, b, a);
                }
            }
        }
        cur_x += adv * scale;
        prev = cp;
    }
}

void blit_icon_rounded(std::vector<uint8_t>& buf, int bw, int bh, const uint8_t* src, int sw, int sh,
                       int ox, int oy, int sz, float radius)
{
    const float hx = sz * 0.5f, hy = sz * 0.5f;
    for (int py = 0; py < sz; py++) {
        const int dy = oy + py;
        if (dy < 0 || dy >= bh) continue;
        const float sy0 = (float)py * sh / sz;
        const float sy1 = (float)(py + 1) * sh / sz;
        const int iy0 = std::clamp((int)std::floor(sy0), 0, sh - 1);
        const int iy1 = std::clamp((int)std::ceil(sy1), iy0 + 1, sh);
        for (int px = 0; px < sz; px++) {
            const int dx = ox + px;
            if (dx < 0 || dx >= bw) continue;
            const float d = sd_round_rect(px + 0.5f, py + 0.5f, hx, hy, hx, hy, radius);
            const float mask = std::clamp(0.5f - d, 0.f, 1.f);
            if (mask <= 0.005f) continue;

            const float sx0 = (float)px * sw / sz;
            const float sx1 = (float)(px + 1) * sw / sz;
            const int ix0 = std::clamp((int)std::floor(sx0), 0, sw - 1);
            const int ix1 = std::clamp((int)std::ceil(sx1), ix0 + 1, sw);
            float sr = 0.f, sg = 0.f, sb = 0.f, sa = 0.f, wt = 0.f;
            for (int y = iy0; y < iy1; y++) {
                const float wy = std::min((float)(y + 1), sy1) - std::max((float)y, sy0);
                for (int x = ix0; x < ix1; x++) {
                    const float wx = std::min((float)(x + 1), sx1) - std::max((float)x, sx0);
                    const float w = wx * wy;
                    const uint8_t* p = src + ((size_t)y * sw + x) * 4;
                    sr += p[0] * w; sg += p[1] * w; sb += p[2] * w; sa += p[3] * w;
                    wt += w;
                }
            }
            if (wt > 0.f) {
                const float inv = 1.f / wt;
                blend_px(&buf[((size_t)dy * bw + dx) * 4],
                         clamp_u8(sr * inv), clamp_u8(sg * inv), clamp_u8(sb * inv),
                         (sa * inv / 255.f) * mask);
            }
            if (d > -1.2f && d < 0.2f)
                blend_px(&buf[((size_t)dy * bw + dx) * 4], 180, 190, 205, 0.22f * mask);
        }
    }
}

}

bool trophy_toast::create(vk_device& d, VkPipelineCache cache)
{
    m_d = &d;
    m_cache = cache;
    const char* env = std::getenv("PS3_TROPHY_POPUP");
    if (env && env[0] == '0') {
        m_disabled = true;
        return true;
    }
    if (const char* sec = std::getenv("PS3_TROPHY_POPUP_S"); sec && *sec) {
        const double s = std::clamp(std::atof(sec), 1.0, 30.0);
        m_hold_ns = (uint64_t)(s * 1e9);
    }

    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_TRY(vkCreateSampler(d.dev, &si, nullptr, &m_sampler));

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 1;
    li.pBindings = &b;
    VK_TRY(vkCreateDescriptorSetLayout(d.dev, &li, nullptr, &m_set));

    const VkPushConstantRange pc{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20 };
    VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &m_set;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pc;
    VK_TRY(vkCreatePipelineLayout(d.dev, &pi, nullptr, &m_layout));

    rsx_spv_blob vs_blob{}, fs_blob{};
    if (rsx_spv_overlay_vs(&vs_blob) != 0 || rsx_spv_overlay_fs(&fs_blob) != 0) {
        rsx_spv_blob_free(&vs_blob);
        rsx_spv_blob_free(&fs_blob);
        return false;
    }
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = (size_t)vs_blob.count * 4u;
    mi.pCode = vs_blob.words;
    VkResult rv = vkCreateShaderModule(d.dev, &mi, nullptr, &m_vs);
    mi.codeSize = (size_t)fs_blob.count * 4u;
    mi.pCode = fs_blob.words;
    VkResult rf = rv == VK_SUCCESS ? vkCreateShaderModule(d.dev, &mi, nullptr, &m_fs) : rv;
    rsx_spv_blob_free(&vs_blob);
    rsx_spv_blob_free(&fs_blob);
    VK_TRY(rf);
    return true;
}

void trophy_toast::destroy()
{
    if (m_chime_stream) {
        SDL_DestroyAudioStream(m_chime_stream);
        m_chime_stream = nullptr;
    }
    if (!m_d) return;
    retire_current();
    for (retired_tex& r : m_retired) {
        if (r.view) vkDestroyImageView(m_d->dev, r.view, nullptr);
        if (r.img) vmaDestroyImage(m_d->vma, r.img, r.mem);
        if (r.stage) vmaDestroyBuffer(m_d->vma, r.stage, r.stage_mem);
    }
    m_retired.clear();
    if (m_pipe) vkDestroyPipeline(m_d->dev, m_pipe, nullptr);
    if (m_vs) vkDestroyShaderModule(m_d->dev, m_vs, nullptr);
    if (m_fs) vkDestroyShaderModule(m_d->dev, m_fs, nullptr);
    if (m_layout) vkDestroyPipelineLayout(m_d->dev, m_layout, nullptr);
    if (m_set) vkDestroyDescriptorSetLayout(m_d->dev, m_set, nullptr);
    if (m_sampler) vkDestroySampler(m_d->dev, m_sampler, nullptr);
    m_pipe = VK_NULL_HANDLE;
    m_vs = m_fs = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_set = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_d = nullptr;
}

bool trophy_toast::ensure_pipeline(VkFormat format)
{
    if (m_pipe && m_pipe_format == format) return true;
    if (m_pipe) {
        {
            std::lock_guard<std::mutex> hold(m_d->queue_lock);
            vkDeviceWaitIdle(m_d->dev);
        }
        vkDestroyPipeline(m_d->dev, m_pipe, nullptr);
        m_pipe = VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, m_vs, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, m_fs, "main" };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = m_layout;
    if (vkCreateGraphicsPipelines(m_d->dev, m_cache, 1, &gi, nullptr, &m_pipe) != VK_SUCCESS) {
        m_pipe = VK_NULL_HANDLE;
        return false;
    }
    m_pipe_format = format;
    return true;
}

void trophy_toast::ensure_font()
{
    if (m_font_tried) return;
    m_font_tried = true;
    static const char* const k_paths[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    };
    for (const char* path : k_paths) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 1024 && n < 32 * 1024 * 1024) {
            m_font_data.resize((size_t)n);
            if (std::fread(m_font_data.data(), 1, (size_t)n, f) == (size_t)n) {
                std::fclose(f);
                return;
            }
            m_font_data.clear();
        }
        std::fclose(f);
    }
}

void trophy_toast::render_card(const pending_item& item, uint32_t ref_h)
{
    ensure_font();
    stbtt_fontinfo font{};
    const bool has_font = !m_font_data.empty() && stbtt_InitFont(&font, m_font_data.data(), stbtt_GetFontOffsetForIndex(m_font_data.data(), 0));

    const float k = std::clamp((float)ref_h / 720.f, 0.75f, 3.0f);
    const int shadow = std::max(4, (int)std::lround(8.f * k));
    const int card_h = (int)std::lround(68.f * k);
    const int pad = (int)std::lround(9.f * k);
    const int icon_sz = card_h - pad * 2;
    const int cup_sz = (int)std::lround(18.f * k);
    const int text_gap = (int)std::lround(11.f * k);
    const int cup_gap = (int)std::lround(6.f * k);
    const int right_pad = (int)std::lround(18.f * k);

    const float head_px = 14.5f * k;
    const float title_px = 16.5f * k;
    const float head_scale = has_font ? stbtt_ScaleForPixelHeight(&font, head_px) : head_px;
    const float title_scale = has_font ? stbtt_ScaleForPixelHeight(&font, title_px) : title_px;

    int f_ascent = 0, f_descent = 0, f_linegap = 0;
    if (has_font) stbtt_GetFontVMetrics(&font, &f_ascent, &f_descent, &f_linegap);
    const int head_asc = (int)std::lround(f_ascent * head_scale);
    const int title_asc = (int)std::lround(f_ascent * title_scale);

    const char* heading = toast_heading();
    const float head_w = measure_text(has_font ? &font : nullptr, head_scale, heading);
    const float title_w = measure_text(has_font ? &font : nullptr, title_scale, item.name.c_str());
    const float text_col_w = std::max(head_w, (float)cup_sz + cup_gap + title_w);

    const int min_card_w = (int)std::lround(290.f * k);
    const int max_card_w = (int)std::lround(460.f * k);
    const int card_w = std::clamp(pad + icon_sz + text_gap + (int)std::ceil(text_col_w) + right_pad, min_card_w, max_card_w);

    const int tex_w = card_w + shadow * 2;
    const int tex_h = card_h + shadow * 2;
    m_tex_w = (uint32_t)tex_w;
    m_tex_h = (uint32_t)tex_h;
    m_built_ref_h = ref_h;
    m_pixels.assign((size_t)tex_w * tex_h * 4u, 0);

    const float cx = shadow + card_w * 0.5f;
    const float cy = shadow + card_h * 0.5f;
    const float hx = card_w * 0.5f;
    const float hy = card_h * 0.5f;
    const float radius = 8.f * k;

    for (int y = 0; y < tex_h; y++) {
        const float vy = std::clamp(((float)(y - shadow) / (float)card_h), 0.f, 1.f);
        for (int x = 0; x < tex_w; x++) {
            uint8_t* dst = &m_pixels[((size_t)y * tex_w + x) * 4];
            const float ds = sd_round_rect(x + 0.5f, y + 0.5f - 2.f * k, cx, cy, hx, hy, radius);
            if (ds > -1.f && ds < (float)shadow) {
                const float t = 1.f - std::clamp(ds / (float)shadow, 0.f, 1.f);
                blend_px(dst, 0, 0, 0, t * t * 0.48f);
            }
            const float d = sd_round_rect(x + 0.5f, y + 0.5f, cx, cy, hx, hy, radius);
            const float body = std::clamp(0.5f - d, 0.f, 1.f);
            if (body <= 0.005f) continue;

            float sheen = vy < 0.48f ? (1.f - vy / 0.48f) * 0.55f + 0.15f : 0.f;
            const uint8_t br = clamp_u8(20.f + 26.f * sheen);
            const uint8_t bg = clamp_u8(23.f + 28.f * sheen);
            const uint8_t bb = clamp_u8(29.f + 32.f * sheen);
            const float ba = 0.90f + 0.03f * vy;
            blend_px(dst, br, bg, bb, ba * body);

            if (d > -2.2f * k && d < -0.6f * k && vy < 0.15f)
                blend_px(dst, 255, 255, 255, 0.14f * body);
            const float border = std::clamp(1.25f - std::fabs(d + 0.6f), 0.f, 1.f);
            if (border > 0.01f)
                blend_px(dst, 148, 158, 174, 0.45f * border * body);
        }
    }

    const int icon_x = shadow + pad;
    const int icon_y = shadow + pad;
    int sw = 0, sh = 0, comp = 0;
    uint8_t* decoded = (!item.png.empty()) ? stbi_load_from_memory(item.png.data(), (int)item.png.size(), &sw, &sh, &comp, 4) : nullptr;
    if (decoded && sw > 0 && sh > 0) {
        blit_icon_rounded(m_pixels, tex_w, tex_h, decoded, sw, sh, icon_x, icon_y, icon_sz, 4.f * k);
        stbi_image_free(decoded);
    } else {
        for (int py = 0; py < icon_sz; py++)
            for (int px = 0; px < icon_sz; px++) {
                const float d = sd_round_rect(px + 0.5f, py + 0.5f, icon_sz * 0.5f, icon_sz * 0.5f, icon_sz * 0.5f, icon_sz * 0.5f, 4.f * k);
                if (d < 0.5f) blend_px(&m_pixels[((size_t)(icon_y + py) * tex_w + (icon_x + px)) * 4], 34, 38, 46, std::clamp(0.5f - d, 0.f, 1.f));
            }
        draw_trophy_cup(m_pixels, tex_w, tex_h, icon_x + icon_sz / 6, icon_y + icon_sz / 6, icon_sz * 2 / 3, item.grade);
    }

    const int text_x = icon_x + icon_sz + text_gap;
    const float max_text_w = (float)(shadow + card_w - right_pad - text_x);
    const float line1_y = shadow + 12.f * k;
    const float line2_y = shadow + 36.f * k;

    if (has_font) {
        draw_text(m_pixels, tex_w, tex_h, &font, head_scale, head_asc, (float)text_x, line1_y, heading, 198, 206, 218, max_text_w);
    }
    draw_trophy_cup(m_pixels, tex_w, tex_h, text_x, (int)std::lround(line2_y + 1.f * k), cup_sz, item.grade);
    if (has_font) {
        draw_text(m_pixels, tex_w, tex_h, &font, title_scale, title_asc, (float)(text_x + cup_sz + cup_gap), line2_y,
                  item.name.c_str(), 255, 255, 255, std::max(24.f, max_text_w - cup_sz - cup_gap));
    }

    m_needs_upload = true;
}

void trophy_toast::retire_current()
{
    if (m_img || m_stage) {
        m_retired.push_back({ m_img, m_mem, m_view, m_stage, m_stage_mem, 0 });
        m_img = VK_NULL_HANDLE;
        m_mem = VK_NULL_HANDLE;
        m_view = VK_NULL_HANDLE;
        m_stage = VK_NULL_HANDLE;
        m_stage_mem = VK_NULL_HANDLE;
    }
}

void trophy_toast::gc_retired()
{
    for (auto it = m_retired.begin(); it != m_retired.end();) {
        if (++it->age >= 6u) {
            if (it->view) vkDestroyImageView(m_d->dev, it->view, nullptr);
            if (it->img) vmaDestroyImage(m_d->vma, it->img, it->mem);
            if (it->stage) vmaDestroyBuffer(m_d->vma, it->stage, it->stage_mem);
            it = m_retired.erase(it);
        } else {
            ++it;
        }
    }
}

void trophy_toast::upload_card(VkCommandBuffer cmd)
{
    m_needs_upload = false;
    retire_current();
    if (!m_tex_w || !m_tex_h || m_pixels.empty()) return;

    const VkDeviceSize bytes = m_pixels.size();
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ba{};
    ba.usage = VMA_MEMORY_USAGE_AUTO;
    ba.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo bai{};
    if (vmaCreateBuffer(m_d->vma, &bi, &ba, &m_stage, &m_stage_mem, &bai) != VK_SUCCESS) return;
    if (!bai.pMappedData) {
        vmaDestroyBuffer(m_d->vma, m_stage, m_stage_mem);
        m_stage = VK_NULL_HANDLE;
        m_stage_mem = nullptr;
        return;
    }
    std::memcpy(bai.pMappedData, m_pixels.data(), bytes);
    vmaFlushAllocation(m_d->vma, m_stage_mem, 0, bytes);

    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = { m_tex_w, m_tex_h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo ia{};
    ia.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m_d->vma, &ii, &ia, &m_img, &m_mem, nullptr) != VK_SUCCESS) {
        retire_current();
        return;
    }

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = m_img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_d->dev, &vi, nullptr, &m_view) != VK_SUCCESS) {
        retire_current();
        return;
    }

    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = m_img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy reg{};
    reg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    reg.imageExtent = { m_tex_w, m_tex_h, 1 };
    vkCmdCopyBufferToImage(cmd, m_stage, m_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);

    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void trophy_toast::play_chime()
{
    poll_chime();
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    }
    if (m_chime_stream) {
        SDL_DestroyAudioStream(m_chime_stream);
        m_chime_stream = nullptr;
    }
    const SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, 48000 };
    m_chime_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!m_chime_stream) return;

    constexpr int sr = 48000;
    constexpr int frames = sr * 75 / 100;
    std::vector<float> pcm((size_t)frames * 2u, 0.f);
    constexpr float pi2 = 6.283185307f;
    for (int i = 0; i < frames; i++) {
        const float t = (float)i / (float)sr;
        float s1 = 0.f;
        {
            const float env = std::min(1.f, t / 0.004f) * std::exp(-t * 7.5f);
            s1 = (std::sin(pi2 * 987.77f * t) * 0.72f + std::sin(pi2 * 1975.53f * t) * 0.28f) * env;
        }
        float s2 = 0.f;
        if (t >= 0.095f) {
            const float t2 = t - 0.095f;
            const float env = std::min(1.f, t2 / 0.004f) * std::exp(-t2 * 5.2f);
            s2 = (std::sin(pi2 * 1318.51f * t2) * 0.70f + std::sin(pi2 * 2637.02f * t2) * 0.30f) * env;
        }
        const float v = (s1 * 0.55f + s2 * 0.75f) * 0.16f;
        pcm[(size_t)i * 2u] = v;
        pcm[(size_t)i * 2u + 1u] = v;
    }
    SDL_PutAudioStreamData(m_chime_stream, pcm.data(), (int)(pcm.size() * sizeof(float)));
    SDL_FlushAudioStream(m_chime_stream);
    SDL_ResumeAudioStreamDevice(m_chime_stream);
    m_chime_end_ns = toast_now_ns() + 900000000ull;
}

void trophy_toast::poll_chime()
{
    if (m_chime_stream && toast_now_ns() >= m_chime_end_ns) {
        SDL_DestroyAudioStream(m_chime_stream);
        m_chime_stream = nullptr;
    }
}

bool trophy_toast::record(VkCommandBuffer cmd, VkImage target, VkImageView target_view, VkExtent2D extent, VkFormat format)
{
    if (!m_d || m_disabled || !target_view || extent.width < 320u || extent.height < 180u) return false;
    if (!m_retired.empty()) gc_retired();
    if (m_chime_stream) poll_chime();

    ps3_trophy_toast_event ev{};
    while (ps3_trophy_poll_toast(&ev)) {
        pending_item item;
        item.id = ev.id;
        item.grade = ev.grade;
        item.name = ev.name;
        if (ev.png && ev.png_size) {
            item.png.assign(ev.png, ev.png + ev.png_size);
        }
        std::free(ev.png);
        m_queue.push_back(std::move(item));
    }

    constexpr uint64_t k_in_ns = 260000000ull;
    constexpr uint64_t k_out_ns = 320000000ull;
    const uint64_t total_ns = k_in_ns + m_hold_ns + k_out_ns;

    if (m_active) {
        const uint64_t elapsed = toast_now_ns() - m_start_ns;
        if (elapsed >= total_ns) {
            m_active = false;
            m_queue.pop_front();
        }
    }
    const uint32_t ref_h = std::min(extent.height, extent.width * 9u / 16u);
    if (!m_active) {
        if (m_queue.empty()) return false;
        render_card(m_queue.front(), ref_h);
        m_start_ns = toast_now_ns();
        m_active = true;
        play_chime();
    } else if (m_built_ref_h != ref_h && !m_queue.empty()) {
        render_card(m_queue.front(), ref_h);
    }

    if (m_needs_upload) upload_card(cmd);
    if (!m_view || !ensure_pipeline(format)) return false;

    const uint64_t t = toast_now_ns() - m_start_ns;
    float e = 1.f;
    if (t < k_in_ns) {
        const float x = (float)t / (float)k_in_ns;
        const float u = 1.f - x;
        e = 1.f - u * u * u;
    } else if (t > k_in_ns + m_hold_ns) {
        const float x = std::clamp((float)(t - k_in_ns - m_hold_ns) / (float)k_out_ns, 0.f, 1.f);
        e = (1.f - x) * (1.f - x);
    }
    if (e <= 0.004f) return false;

    const float k = std::clamp((float)ref_h / 720.f, 0.75f, 3.0f);
    const float margin = 18.f * k;
    const float slide = (1.f - e) * 44.f * k;
    const float px0 = (float)extent.width - (float)m_tex_w - margin + slide;
    const float py0 = margin;
    const float px1 = px0 + (float)m_tex_w;
    const float py1 = py0 + (float)m_tex_h;

    const float pc[5] = {
        px0 / (float)extent.width * 2.f - 1.f,
        py0 / (float)extent.height * 2.f - 1.f,
        px1 / (float)extent.width * 2.f - 1.f,
        py1 / (float)extent.height * 2.f - 1.f,
        e,
    };

    VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.image = target;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo att{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    att.imageView = target_view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, extent };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd, &ri);

    const VkViewport vp{ 0.f, 0.f, (float)extent.width, (float)extent.height, 0.f, 1.f };
    const VkRect2D sc{ { 0, 0 }, extent };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipe);

    const VkDescriptorImageInfo img_info{ m_sampler, m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &img_info;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &w);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    vkCmdEndRendering(cmd);

    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &dep);
    return true;
}

}
