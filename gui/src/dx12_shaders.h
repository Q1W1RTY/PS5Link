// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

namespace dx12_shaders {

// Fullscreen triangle vertex shader + NV12/P010 YUV->RGB with:
// - exact padding-safe UV clamping (prevents D3D12VA 1088/1152 alignment bleed),
// - MPEG-2 / BT.2020 chroma siting + anti-ringing 5-tap Catmull-Rom chroma upsampling,
// - ITU-R BT.2390 EETF HDR10 (PQ) & HLG (with BT.2100 OOTF) -> BT.709 SDR tone mapping
//   with soft gamut compression, or native HDR10 PQ passthrough.
inline constexpr char kColorShader[] = R"(
    Texture2DArray<float> luma : register(t0);
    Texture2DArray<float2> chroma : register(t1);
    SamplerState linearClamp : register(s0);

    cbuffer Conversion : register(b0) {
        float2 visibleRatio;
        float2 maxLumaUv;
        float2 maxChromaUv;
        float2 chromaOffsetUv;
        float2 chromaTexSize;
        float2 invChromaTexSize;
        float lumaOffset;
        float lumaScale;
        float chromaOffset;
        float chromaScale;
        float kr;
        float kb;
        float hdrMode;       // 0 = SDR, 1 = PQ->SDR (BT.2390), 2 = HLG->SDR (BT.2390), 3 = Native HDR10 PQ passthrough
        float hdrPaperWhite; // SDR reference paper white in nits (default 203)
    };

    struct Vertex {
        float4 pos : SV_Position;
        float2 uv : TEXCOORD;
    };

    Vertex vertexMain(uint id : SV_VertexID) {
        Vertex v;
        v.uv = float2((id << 1) & 2, id & 2);
        v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
        return v;
    }

    float pqToLinearScalar(float pq) {
        const float m1 = 0.1593017578125;
        const float m2 = 78.84375;
        const float c1 = 0.8359375;
        const float c2 = 18.8515625;
        const float c3 = 18.6875;
        float p = pow(max(pq, 0.0), 1.0 / m2);
        float num = max(p - c1, 0.0);
        float den = max(c2 - c3 * p, 1e-6);
        return pow(num / den, 1.0 / m1);
    }

    float3 pqToLinear(float3 pq) {
        const float m1 = 0.1593017578125;
        const float m2 = 78.84375;
        const float c1 = 0.8359375;
        const float c2 = 18.8515625;
        const float c3 = 18.6875;
        float3 p = pow(max(pq, 0.0), 1.0 / m2);
        float3 num = max(p - c1, 0.0);
        float3 den = max(c2 - c3 * p, 1e-6);
        return pow(num / den, 1.0 / m1);
    }

    float linearToPqScalar(float y) {
        const float m1 = 0.1593017578125;
        const float m2 = 78.84375;
        const float c1 = 0.8359375;
        const float c2 = 18.8515625;
        const float c3 = 18.6875;
        float ym = pow(max(y, 0.0), m1);
        return pow(saturate((c1 + c2 * ym) / (1.0 + c3 * ym)), m2);
    }

    float3 hlgToLinear(float3 e) {
        const float a = 0.17883277;
        const float b = 0.28466892;
        const float c = 0.55991073;
        float3 lo = (e * e) / 3.0;
        float3 hi = (exp((e - c) / a) + b) / 12.0;
        return lerp(lo, hi, step(0.5, e));
    }

    float3 bt2020ToBt709(float3 c) {
        return float3(
             1.6604910 * c.r - 0.5876411 * c.g - 0.0728499 * c.b,
            -0.1245505 * c.r + 1.1328999 * c.g - 0.0083494 * c.b,
            -0.0181508 * c.r - 0.1005789 * c.g + 1.1187297 * c.b
        );
    }

    float3 linearToSrgb(float3 x) {
        x = saturate(x);
        float3 lo = x * 12.92;
        float3 hi = 1.055 * pow(max(x, 1e-6), 1.0 / 2.4) - 0.055;
        return lerp(lo, hi, step(0.0031308, x));
    }

    // Anti-ringing 5-tap Catmull-Rom chroma upsampling with siting offset and padding-safe clamp.
    float2 sampleChromaBicubic(float2 baseUv) {
        float2 minUv = 0.5 * invChromaTexSize;
        float2 pos = clamp(baseUv + chromaOffsetUv, minUv, maxChromaUv);
        float2 tc = pos * chromaTexSize - 0.5;
        float2 f = frac(tc);
        float2 t0 = floor(tc) + 0.5;
        float2 f2 = f * f;
        float2 f3 = f2 * f;

        float2 w0 = -0.5 * f + f2 - 0.5 * f3;
        float2 w1 = 1.0 - 2.5 * f2 + 1.5 * f3;
        float2 w2 = 0.5 * f + 2.0 * f2 - 1.5 * f3;
        float2 w3 = -0.5 * f2 + 0.5 * f3;
        float2 w12 = w1 + w2;

        float2 t12 = (t0 + w2 / max(w12, 1e-5)) * invChromaTexSize;
        float2 tm1 = (t0 - 1.0) * invChromaTexSize;
        float2 tp2 = (t0 + 2.0) * invChromaTexSize;

        float2 cC = chroma.Sample(linearClamp, float3(clamp(t12, minUv, maxChromaUv), 0));
        float2 cL = chroma.Sample(linearClamp, float3(clamp(float2(tm1.x, t12.y), minUv, maxChromaUv), 0));
        float2 cR = chroma.Sample(linearClamp, float3(clamp(float2(tp2.x, t12.y), minUv, maxChromaUv), 0));
        float2 cT = chroma.Sample(linearClamp, float3(clamp(float2(t12.x, tm1.y), minUv, maxChromaUv), 0));
        float2 cB = chroma.Sample(linearClamp, float3(clamp(float2(t12.x, tp2.y), minUv, maxChromaUv), 0));

        float wC = w12.x * w12.y;
        float wL = w0.x * w12.y;
        float wR = w3.x * w12.y;
        float wT = w12.x * w0.y;
        float wB = w12.x * w3.y;
        float2 result = (cC * wC + cL * wL + cR * wR + cT * wT + cB * wB) / max(wC + wL + wR + wT + wB, 1e-5);
        float2 mn = min(cC, min(min(cL, cR), min(cT, cB)));
        float2 mx = max(cC, max(max(cL, cR), max(cT, cB)));
        return clamp(result, mn, mx);
    }

    // ITU-R BT.2390 EETF luminance tone mapping + soft BT.2020 -> BT.709 gamut compression.
    // Preserves SDR midtone brightness 1:1 and avoids ACES hue shifts.
    float3 toneMapBt2390(float3 linNits, float srcPeakNits, float paperWhiteNits) {
        float yNits = max(dot(linNits, float3(0.2627, 0.6780, 0.0593)), 0.0);
        float maxPq = linearToPqScalar(srcPeakNits / 10000.0);
        float dstPq = linearToPqScalar(paperWhiteNits / 10000.0);
        float E1 = linearToPqScalar(min(yNits, srcPeakNits) / 10000.0) / max(maxPq, 1e-5);
        float maxLum = dstPq / max(maxPq, 1e-5);
        float ks = max(1.5 * maxLum - 0.5, 0.0);
        float E2 = E1;
        if (E1 > ks) {
            float b = saturate((E1 - ks) / max(1.0 - ks, 1e-5));
            float b2 = b * b;
            float b3 = b2 * b;
            E2 = (2.0 * b3 - 3.0 * b2 + 1.0) * ks
               + (b3 - 2.0 * b2 + b) * (1.0 - ks)
               + (-2.0 * b3 + 3.0 * b2) * maxLum;
        }
        float yOutNits = pqToLinearScalar(E2 * maxPq) * 10000.0;
        float3 mapped = (yNits > 1e-5) ? (linNits * (yOutNits / yNits)) : 0.0;
        float invWhite = 1.0 / max(paperWhiteNits, 1.0);
        mapped *= invWhite;
        float yNorm = yOutNits * invWhite;

        // Smoothly roll off super-bright single-channel peaks toward luminance to avoid clipping hue.
        float peakC = max(mapped.r, max(mapped.g, mapped.b));
        if (peakC > 1.0) {
            float desat = saturate((peakC - 1.0) / (peakC + 0.5)) * 0.5;
            mapped = lerp(mapped / peakC, float3(yNorm, yNorm, yNorm), desat);
        }

        // Soft BT.2020 -> BT.709 gamut compression (pulls only negative channels toward BT.709 luma).
        float3 rec709 = bt2020ToBt709(mapped);
        float l709 = saturate(dot(rec709, float3(0.2126, 0.7152, 0.0722)));
        float minC = min(rec709.r, min(rec709.g, rec709.b));
        if (minC < 0.0) {
            float t = (-minC) / max(l709 - minC, 1e-5);
            rec709 = lerp(rec709, float3(l709, l709, l709), saturate(t));
        }
        return linearToSrgb(saturate(rec709));
    }

    float4 pixelMain(Vertex v) : SV_Target {
        float2 baseUv = v.uv * visibleRatio;
        float2 lumaUv = clamp(baseUv, float2(0.0, 0.0), maxLumaUv);
        float y = (luma.Sample(linearClamp, float3(lumaUv, 0)) - lumaOffset) * lumaScale;
        float2 cbcr = (sampleChromaBicubic(baseUv) - chromaOffset) * chromaScale;
        float r = y + 2.0 * (1.0 - kr) * cbcr.y;
        float b = y + 2.0 * (1.0 - kb) * cbcr.x;
        float g = (y - kr * r - kb * b) / (1.0 - kr - kb);
        float3 rgb = saturate(float3(r, g, b));

        if (hdrMode > 0.5 && hdrMode < 2.5) {
            if (hdrMode < 1.5) {
                float3 linNits = pqToLinear(rgb) * 10000.0;
                rgb = toneMapBt2390(linNits, 4000.0, hdrPaperWhite);
            } else {
                // ITU-R BT.2100 HLG inverse OETF + system gamma OOTF (gamma ~1.2 for 1000 nit reference).
                float3 e = hlgToLinear(rgb);
                float ys = dot(e, float3(0.2627, 0.6780, 0.0593));
                float3 linNits = e * pow(max(ys, 1e-6), 0.2) * 1000.0;
                rgb = toneMapBt2390(linNits, 1000.0, hdrPaperWhite);
            }
        }
        return float4(rgb, 1.0);
    }
)";

// Simple 1:1 / viewport-fitted blit shader for compositing upscaled textures across aspect ratios or formats.
inline constexpr char kBlitShader[] = R"(
    Texture2D<float4> source : register(t0);
    SamplerState linearClamp : register(s0);
    struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD; };

    float4 blit(Vertex v) : SV_Target {
        return float4(source.Sample(linearClamp, v.uv).rgb, 1.0);
    }
)";

// AMD FSR 1.0 EASU-inspired 12-tap Edge-Adaptive Spatial Upscaler (fast ~0.3ms high-clarity alternative to bilinear).
inline constexpr char kFsrEasuShader[] = R"(
    Texture2D<float4> source : register(t0);
    SamplerState linearClamp : register(s0);
    cbuffer Geometry : register(b0) {
        float2 inputSize;
        float2 invInputSize;
    };
    struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD; };

    float luma(float3 c) { return dot(c, float3(0.25, 0.50, 0.25)); }

    void easuSet(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV,
                 float lA, float lB, float lC, float lD, float lE) {
        float w = 0.0;
        if (biS) w = (1.0 - pp.x) * (1.0 - pp.y);
        if (biT) w = pp.x * (1.0 - pp.y);
        if (biU) w = (1.0 - pp.x) * pp.y;
        if (biV) w = pp.x * pp.y;

        float dc = lD - lC;
        float cb = lC - lB;
        float lenX = max(abs(dc), abs(cb));
        lenX = (lenX > 1e-5) ? rcp(lenX) : 0.0;
        float dirX = lD - lB;
        dir.x += dirX * w;
        lenX = saturate(abs(dirX) * lenX);
        lenX *= lenX;
        len += lenX * w;

        float ec = lE - lC;
        float ca = lC - lA;
        float lenY = max(abs(ec), abs(ca));
        lenY = (lenY > 1e-5) ? rcp(lenY) : 0.0;
        float dirY = lE - lA;
        dir.y += dirY * w;
        lenY = saturate(abs(dirY) * lenY);
        lenY *= lenY;
        len += lenY * w;
    }

    void easuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
        float2 v;
        v.x = (off.x * dir.x) + (off.y * dir.y);
        v.y = (off.x * (-dir.y)) + (off.y * dir.x);
        v *= len;
        float d2 = min(dot(v, v), clp);
        // Fast Lanczos2 approximation from AMD FidelityFX FSR 1.0
        float wB = 0.4 * d2 - 1.0;
        float wA = lob * d2 - 1.0;
        wB *= wB;
        wA *= wA;
        wB = 1.5625 * wB - 0.5625;
        float w = wB * wA;
        aC += c * w;
        aW += w;
    }

    float4 fsrEasu(Vertex v) : SV_Target {
        float2 pp = v.uv * inputSize - 0.5;
        float2 fp = floor(pp);
        pp -= fp;

        int2 iMax = int2(inputSize) - 1;
        int2 p = int2(fp);
        #define FETCH(dx, dy) source.Load(int3(clamp(p + int2(dx, dy), int2(0, 0), iMax), 0)).rgb
        float3 b = FETCH( 0, -1); float3 c = FETCH( 1, -1);
        float3 e = FETCH(-1,  0); float3 f = FETCH( 0,  0); float3 g = FETCH( 1,  0); float3 h = FETCH( 2,  0);
        float3 i = FETCH(-1,  1); float3 j = FETCH( 0,  1); float3 k = FETCH( 1,  1); float3 l = FETCH( 2,  1);
        float3 n = FETCH( 0,  2); float3 o = FETCH( 1,  2);
        #undef FETCH

        float bL = luma(b), cL = luma(c);
        float eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
        float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l);
        float nL = luma(n), oL = luma(o);

        float2 dir = 0.0;
        float len = 0.0;
        easuSet(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
        easuSet(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
        easuSet(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
        easuSet(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);

        float2 dir2 = dir * dir;
        float dirR = dir2.x + dir2.y;
        bool zro = dirR < (1.0 / 32768.0);
        dirR = zro ? 1.0 : rsqrt(dirR);
        dir = zro ? float2(1.0, 0.0) : (dir * dirR);

        len *= 0.5;
        len *= len;
        float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
        float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
        float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
        float clp = rcp(lob);

        float3 min4 = min(min(f, g), min(j, k));
        float3 max4 = max(max(f, g), max(j, k));

        float3 aC = 0.0;
        float aW = 0.0;
        easuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
        easuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
        easuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
        easuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
        easuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
        easuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
        easuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
        easuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
        easuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
        easuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
        easuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);
        easuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);

        float3 color = clamp(aC / max(aW, 1e-5), min4, max4);
        return float4(color, 1.0);
    }
)";

// Pyramidal hierarchical motion estimation (+/-60 px range):
// 1/4-res anti-aliased luma pyramid -> 3x3 coarse block search -> 1/8-res spatial propagation
// & sub-pixel diamond refinement -> confidence-filtered warp / NGX & FFX motion vectors.
inline constexpr char kInterpolationShader[] = R"(
    Texture2D<float4> previousFrame : register(t0);
    Texture2D<float4> currentFrame : register(t1);
    Texture2D<float4> motion : register(t2);
    SamplerState linearClamp : register(s0);
    cbuffer Geometry : register(b0) {
        float2 pixelSize;
        float2 motionPixelSize;
    };
    struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD; };

    float luma(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

    // 4x4 box-filtered luma downsample to 1/4 resolution for alias-free coarse block matching.
    float downsampleLuma(Vertex v) : SV_Target {
        float3 c0 = previousFrame.SampleLevel(linearClamp, v.uv + float2(-1.0, -1.0) * pixelSize, 0).rgb;
        float3 c1 = previousFrame.SampleLevel(linearClamp, v.uv + float2( 1.0, -1.0) * pixelSize, 0).rgb;
        float3 c2 = previousFrame.SampleLevel(linearClamp, v.uv + float2(-1.0,  1.0) * pixelSize, 0).rgb;
        float3 c3 = previousFrame.SampleLevel(linearClamp, v.uv + float2( 1.0,  1.0) * pixelSize, 0).rgb;
        return luma((c0 + c1 + c2 + c3) * 0.25);
    }

    // 3x3 block SAD on pre-filtered 1/4-res luma textures (covers 12x12 full-res block without aliasing).
    float matchBlockCoarse(float2 uv, float2 shift) {
        float error = 0.0;
        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                float2 loc = uv + float2(dx, dy) * pixelSize;
                float oldL = previousFrame.SampleLevel(linearClamp, loc - shift, 0).r;
                float newL = currentFrame.SampleLevel(linearClamp, loc + shift, 0).r;
                error += abs(oldL - newL);
            }
        }
        return error * (1.0 / 9.0);
    }

    // 5-tap cross SAD on full-res RGB frames for fine sub-pixel refinement.
    float matchCross(float2 uv, float2 shift, float stride) {
        float error = 0;
        const float2 offsets[5] = {
            float2(0, 0),
            float2(-stride, 0), float2(stride, 0),
            float2(0, -stride), float2(0, stride)
        };
        [unroll] for (int i = 0; i < 5; ++i) {
            float2 location = uv + offsets[i] * pixelSize;
            float oldLuma = luma(previousFrame.SampleLevel(linearClamp, location - shift, 0).rgb);
            float newLuma = luma(currentFrame.SampleLevel(linearClamp, location + shift, 0).rgb);
            error += abs(oldLuma - newLuma);
        }
        return error * 0.2;
    }

    float4 estimateMotionCoarse(Vertex v) : SV_Target {
        float2 best = 0;
        float bestCost = matchBlockCoarse(v.uv, 0);
        // Steps in 1/4-res pixels = {16, 8, 4, 2} full-res half-shifts -> +/-60 px full range.
        const float steps[4] = { 4.0, 2.0, 1.0, 0.5 };
        [unroll] for (int level = 0; level < 4; ++level) {
            float step = steps[level];
            float2 center = best;
            [unroll] for (int y = -1; y <= 1; ++y) {
                [unroll] for (int x = -1; x <= 1; ++x) {
                    if (x == 0 && y == 0) continue;
                    float2 candidate = center + float2(x, y) * step;
                    float cost = matchBlockCoarse(v.uv, candidate * pixelSize)
                        + dot(abs(candidate), float2(0.0012, 0.0012));
                    if (cost < bestCost) { bestCost = cost; best = candidate; }
                }
            }
        }
        return float4(best * 4.0, bestCost, 1);
    }

    float4 refineMotion(Vertex v) : SV_Target {
        float2 best = motion.SampleLevel(linearClamp, v.uv, 0).xy;
        float bestCost = matchCross(v.uv, best * pixelSize, 2.0) + dot(abs(best), float2(0.00025, 0.00025));

        const float2 neighbors[4] = {
            float2(-motionPixelSize.x, 0), float2(motionPixelSize.x, 0),
            float2(0, -motionPixelSize.y), float2(0, motionPixelSize.y)
        };
        float zeroCost = matchCross(v.uv, 0, 2.0);
        if (zeroCost < bestCost) { bestCost = zeroCost; best = 0; }

        [unroll] for (int n = 0; n < 4; ++n) {
            float2 cand = motion.SampleLevel(linearClamp, v.uv + neighbors[n], 0).xy;
            float cost = matchCross(v.uv, cand * pixelSize, 2.0) + dot(abs(cand), float2(0.00025, 0.00025));
            if (cost < bestCost) { bestCost = cost; best = cand; }
        }

        const float steps[3] = { 2.0, 1.0, 0.5 };
        const float2 dirs[4] = { float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1) };
        [unroll] for (int level = 0; level < 3; ++level) {
            float step = steps[level];
            float2 center = best;
            [unroll] for (int d = 0; d < 4; ++d) {
                float2 candidate = center + dirs[d] * step;
                float cost = matchCross(v.uv, candidate * pixelSize, 2.0)
                    + dot(abs(candidate), float2(0.00025, 0.00025));
                if (cost < bestCost) { bestCost = cost; best = candidate; }
            }
        }
        return float4(best, bestCost, 1);
    }

    float3 filteredFlow(float2 uv) {
        float3 c = motion.SampleLevel(linearClamp, uv, 0).xyz;
        float3 l = motion.SampleLevel(linearClamp, uv + float2(-motionPixelSize.x, 0), 0).xyz;
        float3 r = motion.SampleLevel(linearClamp, uv + float2( motionPixelSize.x, 0), 0).xyz;
        float3 u = motion.SampleLevel(linearClamp, uv + float2(0, -motionPixelSize.y), 0).xyz;
        float3 d = motion.SampleLevel(linearClamp, uv + float2(0,  motionPixelSize.y), 0).xyz;
        float wc = 2.0 / (0.02 + c.z);
        float wl = 1.0 / (0.02 + l.z);
        float wr = 1.0 / (0.02 + r.z);
        float wu = 1.0 / (0.02 + u.z);
        float wd = 1.0 / (0.02 + d.z);
        float wsum = wc + wl + wr + wu + wd;
        return (c * wc + l * wl + r * wr + u * wu + d * wd) / wsum;
    }

    float4 interpolate(Vertex v) : SV_Target {
        float3 flow = filteredFlow(v.uv);
        float2 shift = flow.xy * pixelSize;
        float3 before = previousFrame.SampleLevel(linearClamp, v.uv - shift, 0).rgb;
        float3 after = currentFrame.SampleLevel(linearClamp, v.uv + shift, 0).rgb;
        float mismatch = max(flow.z, abs(luma(before) - luma(after)));
        float3 current = currentFrame.SampleLevel(linearClamp, v.uv, 0).rgb;
        return float4(lerp((before + after) * 0.5, current, smoothstep(0.05, 0.16, mismatch)), 1);
    }

    float2 dlssMotion(Vertex v) : SV_Target {
        float3 flow = filteredFlow(v.uv);
        // The search estimates half the forward displacement. NGX & FFX expect current-to-previous pixels.
        // Zero out vectors on scene cuts / high-mismatch disocclusions to prevent streaking.
        float confidence = 1.0 - smoothstep(0.07, 0.20, flow.z);
        return -2.0 * flow.xy * confidence;
    }
)";

// Final-stage Post-Processing:
// Optional 4-tap Stochastic Deband (H.265 gradient smoothing) + Contrast-Adaptive Sharpening (CAS)
// + Color Grading + High-Precision Temporal Triangular IGN Dithering + Fair A/B Split-Screen.
inline constexpr char kPostProcessShader[] = R"(
    Texture2D<float4> enhanced : register(t0);
    Texture2D<float4> baseline : register(t1);
    cbuffer PostParams : register(b0) {
        float sharpness;
        float splitX;
        float splitEnabled;
        float ditherEnabled;
        float contrast;
        float saturation;
        float gamma;
        float frameSeed;
        float debandEnabled;
        float ditherScale;
        float hdrPassthrough;
        float pad0;
    };

    float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

    float ign(float2 p) {
        return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
    }

    // Lightweight 4-tap detail-preserving deband filter (erases 8-bit H.265 banding in skies/shadows).
    float3 applyDeband(int2 coord, int2 maxCoord, float3 center) {
        if (debandEnabled < 0.5 || hdrPassthrough > 0.5)
            return center;
        float h1 = ign(float2(coord) + float2(frameSeed * 5.7, frameSeed * 3.1));
        float h2 = ign(float2(coord) + float2(frameSeed * 9.3 + 11.0, frameSeed * 7.9 + 4.0));
        float angle = h1 * 6.2831853;
        float radius = (0.35 + 0.65 * h2) * 14.0;
        float s, c;
        sincos(angle, s, c);
        int2 d1 = int2(round(float2(c, s) * radius));
        int2 d2 = int2(-d1.y, d1.x);

        float3 p0 = enhanced.Load(int3(clamp(coord + d1, int2(0, 0), maxCoord), 0)).rgb;
        float3 p1 = enhanced.Load(int3(clamp(coord - d1, int2(0, 0), maxCoord), 0)).rgb;
        float3 p2 = enhanced.Load(int3(clamp(coord + d2, int2(0, 0), maxCoord), 0)).rgb;
        float3 p3 = enhanced.Load(int3(clamp(coord - d2, int2(0, 0), maxCoord), 0)).rgb;

        float3 avg = (p0 + p1 + p2 + p3) * 0.25;
        float3 maxDiff = max(abs(center - avg),
            max(max(abs(center - p0), abs(center - p1)), max(abs(center - p2), abs(center - p3))));
        float diff = max(maxDiff.r, max(maxDiff.g, maxDiff.b));
        // Only blend inside low-contrast flat gradients (< ~5.5 / 255).
        float blend = 1.0 - smoothstep(0.010, 0.024, diff);
        return lerp(center, avg, blend);
    }

    float3 applyCas(int2 coord, int2 maxCoord, float3 c) {
        if (sharpness <= 0.001)
            return c;
        float3 n = enhanced.Load(int3(clamp(coord + int2(0, -1), int2(0, 0), maxCoord), 0)).rgb;
        float3 s = enhanced.Load(int3(clamp(coord + int2(0,  1), int2(0, 0), maxCoord), 0)).rgb;
        float3 w = enhanced.Load(int3(clamp(coord + int2(-1, 0), int2(0, 0), maxCoord), 0)).rgb;
        float3 e = enhanced.Load(int3(clamp(coord + int2( 1, 0), int2(0, 0), maxCoord), 0)).rgb;

        float lc = luma(c);
        float ln = luma(n);
        float ls = luma(s);
        float lw = luma(w);
        float le = luma(e);

        float minL = min(lc, min(min(ln, ls), min(lw, le)));
        float maxL = max(lc, max(max(ln, ls), max(lw, le)));
        float amp = sqrt(saturate(min(minL, 1.0 - maxL) / max(maxL, 1e-4)));
        float weight = -sharpness * 0.2 * amp;
        float3 sharpened = (c + (n + s + w + e) * weight) / (1.0 + 4.0 * weight);
        return saturate(sharpened);
    }

    float3 applyColorGrading(float3 c) {
        if (hdrPassthrough > 0.5)
            return c;
        if (abs(gamma - 1.0) > 0.005)
            c = pow(max(c, 1e-6), 1.0 / max(gamma, 0.25));
        if (abs(contrast - 1.0) > 0.005)
            c = saturate((c - 0.5) * contrast + 0.5);
        if (abs(saturation - 1.0) > 0.005) {
            float l = luma(c);
            // Vibrance-aware saturation: protects already-saturated tones from clipping.
            float mx = max(c.r, max(c.g, c.b));
            float mn = min(c.r, min(c.g, c.b));
            float sat = mx - mn;
            float boost = (saturation > 1.0) ? lerp(saturation, 1.0, sat * 0.5) : saturation;
            c = saturate(lerp(float3(l, l, l), c, boost));
        }
        return c;
    }

    float3 applyDither(float3 c, float2 pos) {
        if (ditherEnabled < 0.5)
            return c;
        // Temporal triangular-PDF dither scaled to target bit depth (8-bit or 10-bit).
        float r1 = ign(pos + float2(frameSeed * 13.0, frameSeed * 7.0));
        float r2 = ign(pos + float2(frameSeed * 17.0 + 5.3, frameSeed * 11.0 + 2.7));
        float tri = (r1 + r2) - 1.0;
        return saturate(c + tri * ditherScale);
    }

    float4 postProcess(float4 position : SV_Position) : SV_Target {
        uint width, height;
        enhanced.GetDimensions(width, height);
        int2 coord = clamp(int2(position.xy), int2(0, 0), int2(width - 1, height - 1));
        int2 maxCoord = int2(width - 1, height - 1);

        if (splitEnabled > 0.5) {
            float divider = splitX * float(width);
            if (abs(position.x - divider) < 1.5)
                return float4(0.47, 0.87, 0.81, 1.0);
            if (position.x < divider) {
                // Fair comparison: apply identical color grading & dithering to baseline side.
                float3 baseCol = baseline.Load(int3(coord, 0)).rgb;
                baseCol = applyColorGrading(baseCol);
                baseCol = applyDither(baseCol, position.xy);
                return float4(baseCol, 1.0);
            }
        }
        float3 col = enhanced.Load(int3(coord, 0)).rgb;
        col = applyDeband(coord, maxCoord, col);
        col = applyCas(coord, maxCoord, col);
        col = applyColorGrading(col);
        col = applyDither(col, position.xy);
        return float4(col, 1.0);
    }
)";

// Linear-light anti-ringing Lanczos2 downsampler for 1800p/2160p -> display output (up to 2x ratio).
// Uses vertex UV so it works in any letterboxed/pillarboxed viewport on ultrawide/16:10 displays.
inline constexpr char kDownsampleShader[] = R"(
    Texture2D<float4> source : register(t0);
    cbuffer Geometry : register(b0) { float2 sourcePixelsPerOutputPixel; };
    struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD; };

    float3 srgbToLinear(float3 x) {
        x = saturate(x);
        return lerp(x / 12.92, pow(max((x + 0.055) / 1.055, 1e-6), 2.4), step(0.04045, x));
    }

    float3 linearToSrgb(float3 x) {
        x = saturate(x);
        return lerp(x * 12.92, 1.055 * pow(max(x, 1e-6), 1.0 / 2.4) - 0.055, step(0.0031308, x));
    }

    float lanczos2(float x) {
        x = abs(x);
        if (x < 1e-4) return 1.0;
        if (x >= 2.0) return 0.0;
        float px = 3.14159265 * x;
        return (sin(px) / px) * (sin(px * 0.5) / (px * 0.5));
    }

    float4 downsample(Vertex v) : SV_Target {
        uint width, height;
        source.GetDimensions(width, height);
        int2 maxCoord = int2(width - 1, height - 1);
        float2 srcPos = v.uv * float2(width, height) - 0.5;
        int2 center = int2(floor(srcPos));
        float2 invScale = 1.0 / max(sourcePixelsPerOutputPixel, float2(1.0, 1.0));

        float3 acc = 0.0;
        float weightSum = 0.0;
        float3 minLin = 1e6;
        float3 maxLin = -1e6;

        [unroll] for (int dy = -3; dy <= 4; ++dy) {
            float wy = lanczos2((float(center.y + dy) - srcPos.y) * invScale.y);
            [unroll] for (int dx = -3; dx <= 4; ++dx) {
                float wx = lanczos2((float(center.x + dx) - srcPos.x) * invScale.x);
                float w = wx * wy;
                int2 tc = clamp(center + int2(dx, dy), int2(0, 0), maxCoord);
                float3 lin = srgbToLinear(source.Load(int3(tc, 0)).rgb);
                acc += lin * w;
                weightSum += w;
                if (abs(dx) <= 1 && abs(dy) <= 1) {
                    minLin = min(minLin, lin);
                    maxLin = max(maxLin, lin);
                }
            }
        }
        float3 linOut = clamp(acc / max(weightSum, 1e-5), minLin, maxLin);
        return float4(linearToSrgb(linOut), 1.0);
    }
)";

// 3-mode ambient background for non-16:9 letterboxed/pillarboxed windows:
// style 0 = Ambient colors (soft edge glow), 1 = Blurred video, 2 = Edge stretch.
inline constexpr char kAmbientShader[] = R"(
    Texture2D<float4> source : register(t0);
    SamplerState linearClamp : register(s0);
    cbuffer Params : register(b0) {
        float2 blurStep;
        float style;
        float pad;
        float4 videoRect; // (left, top, right, bottom) in [0..1]
    };
    struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD; };

    float4 ambientBg(Vertex v) : SV_Target {
        float2 rectSize = max(videoRect.zw - videoRect.xy, float2(1e-4, 1e-4));
        float2 localUv = (v.uv - videoRect.xy) / rectSize;
        float2 clamped = clamp(localUv, float2(0.004, 0.004), float2(0.996, 0.996));
        float dist = length(localUv - clamped);

        if (style < 0.5) {
            // Mode 0: Ambient colors — wide soft color glow sampled near the video border.
            float2 stepWide = max(blurStep * 3.0, float2(0.025, 0.025));
            float3 acc = 0;
            [unroll] for (int dy = -1; dy <= 1; ++dy) {
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    float2 sUv = clamp(clamped + float2(dx, dy) * stepWide, float2(0.01, 0.01), float2(0.99, 0.99));
                    acc += source.SampleLevel(linearClamp, sUv, 0).rgb;
                }
            }
            float falloff = exp(-dist * 2.4) * 0.65 + 0.12;
            return float4((acc / 9.0) * falloff, 1.0);
        }

        if (style > 1.5) {
            // Mode 2: Edge stretch — extend outer edge pixels seamlessly into the bars.
            float2 stepSoft = blurStep * 0.75;
            float3 c0 = source.SampleLevel(linearClamp, clamped, 0).rgb;
            float3 c1 = source.SampleLevel(linearClamp, clamp(clamped + float2(0, stepSoft.y), 0.004, 0.996), 0).rgb;
            float3 c2 = source.SampleLevel(linearClamp, clamp(clamped - float2(0, stepSoft.y), 0.004, 0.996), 0).rgb;
            float3 c3 = source.SampleLevel(linearClamp, clamp(clamped + float2(stepSoft.x, 0), 0.004, 0.996), 0).rgb;
            float3 c4 = source.SampleLevel(linearClamp, clamp(clamped - float2(stepSoft.x, 0), 0.004, 0.996), 0).rgb;
            float3 edge = (c0 * 2.0 + c1 + c2 + c3 + c4) / 6.0;
            float dim = lerp(0.82, 0.45, saturate(dist * 2.5));
            return float4(edge * dim, 1.0);
        }

        // Mode 1: Blurred video — zoomed heavily blurred copy of the frame.
        const float2 offsets[9] = {
            float2(0, 0),
            float2(-1.5, -1.5), float2(0.0, -2.0), float2(1.5, -1.5),
            float2(-2.0,  0.0),                   float2(2.0,  0.0),
            float2(-1.5,  1.5), float2(0.0,  2.0), float2(1.5,  1.5)
        };
        float3 acc = 0;
        [unroll] for (int i = 0; i < 9; ++i)
            acc += source.SampleLevel(linearClamp, v.uv + offsets[i] * blurStep, 0).rgb;
        return float4((acc / 9.0) * 0.42, 1.0);
    }
)";

} // namespace dx12_shaders
