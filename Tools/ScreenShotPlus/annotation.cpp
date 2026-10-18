#include "annotation.h"
using namespace std;
using namespace Gdiplus;
namespace {
RectF ShapeRect(const Annotation& a) {
    if (a.points.size() < 2) return {};
    const auto p = a.points.front(), q = a.points.back();
    return {min(p.X, q.X), min(p.Y, q.Y), abs(p.X - q.X), abs(p.Y - q.Y)};
}
float SegmentDistance(PointF p, PointF a, PointF b) {
    const float x = b.X - a.X, y = b.Y - a.Y, d = x * x + y * y;
    const float t = d ? clamp(((p.X - a.X) * x + (p.Y - a.Y) * y) / d, 0.0f, 1.0f) : 0;
    return hypot(p.X - a.X - t * x, p.Y - a.Y - t * y);
}
RectF WatermarkText(Graphics& g, const Annotation& a) {
    Font font(L"Microsoft YaHei UI", a.size, FontStyleRegular, UnitPixel); RectF box;
    g.MeasureString(a.text.c_str(), static_cast<INT>(a.text.size()), &font, PointF(0,0), StringFormat::GenericTypographic(), &box);
    return box;
}
void DrawWatermark(Graphics& g, const Annotation& a) {
    if (a.text.empty() || a.extent.Width <= 0 || a.extent.Height <= 0) return;
    const auto saved = g.Save(); g.SetClip(RectF(0,0,a.extent.Width,a.extent.Height), CombineModeIntersect);
    const auto box = WatermarkText(g,a);
    Font font(L"Microsoft YaHei UI", a.size, FontStyleRegular, UnitPixel);
    SolidBrush ink(Color(static_cast<BYTE>(lround(a.color.GetA()*clamp(a.opacity,0.0f,1.0f))), a.color.GetR(), a.color.GetG(), a.color.GetB()));
    const auto stamp = [&](float x,float y) {
        const auto state = g.Save(); g.TranslateTransform(x,y); g.RotateTransform(a.angle);
        g.DrawString(a.text.c_str(),static_cast<INT>(a.text.size()),&font,PointF(-box.Width/2,-box.Height/2),StringFormat::GenericTypographic(),&ink); g.Restore(state);
    };
    if (!a.tiled) stamp(a.points[0].X,a.points[0].Y);
    else {
        float sx=max(a.size,box.Width+max(16.0f,a.spacing)),sy=max(a.size,box.Height+max(16.0f,a.spacing));
        const float count=(a.extent.Width/sx+4)*(a.extent.Height/sy+4);
        if(count>4096){const float factor=sqrt(count/4096);sx*=factor;sy*=factor;}
        const int firstRow=static_cast<int>(floor((-sy-a.points[0].Y)/sy));
        for(int row=firstRow;;++row){
            const float y=a.points[0].Y+row*sy;if(y>=a.extent.Height+sy)break;
            const float anchor=a.points[0].X+(row&1)*sx/2;
            for(float x=anchor+floor((-sx-anchor)/sx)*sx;x<a.extent.Width+sx;x+=sx)stamp(x,y);
        }
    }
    g.Restore(saved);
}
}
void DrawAnnotation(Graphics& g, const Annotation& a) {
    if (a.points.empty() || a.kind == Annotation::Mosaic) return;
    if (a.kind == Annotation::Watermark) { DrawWatermark(g,a); return; }
    if (a.kind == Annotation::Text) {
        Font font(L"Microsoft YaHei UI", a.size, FontStyleRegular, UnitPixel);
        SolidBrush brush(a.color);
        g.DrawString(a.text.c_str(), static_cast<INT>(a.text.size()), &font, a.points[0], StringFormat::GenericTypographic(), &brush);
        return;
    }
    if (a.points.size() == 1) {
        SolidBrush brush(a.color);
        g.FillEllipse(&brush, a.points[0].X - a.size / 2, a.points[0].Y - a.size / 2, a.size, a.size);
        return;
    }
    Pen pen(a.color, a.size); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    if (a.kind == Annotation::Stroke) g.DrawLines(&pen, a.points.data(), static_cast<INT>(a.points.size()));
    else if (a.kind == Annotation::Rectangle) g.DrawRectangle(&pen, ShapeRect(a));
    else if (a.kind == Annotation::Arrow) {
        const auto p = a.points.front(), q = a.points.back();
        const float length = hypot(q.X - p.X, q.Y - p.Y);
        if (length < 0.01f) return;
        const float head = min(length, max(10.0f, a.size * 4)), ux = (q.X - p.X) / length, uy = (q.Y - p.Y) / length;
        g.DrawLine(&pen, p, q);
        const PointF triangle[]{q, {q.X - ux * head - uy * head * .48f, q.Y - uy * head + ux * head * .48f},
            {q.X - ux * head + uy * head * .48f, q.Y - uy * head - ux * head * .48f}};
        SolidBrush brush(a.color); g.FillPolygon(&brush, triangle, 3);
    }
}
RectF AnnotationTextBounds(Graphics& g, const Annotation& a) {
    RectF result;
    if (a.kind != Annotation::Text || a.points.empty()) return result;
    Font font(L"Microsoft YaHei UI", a.size, FontStyleRegular, UnitPixel);
    g.MeasureString(a.text.c_str(), static_cast<INT>(a.text.size()), &font, a.points[0], StringFormat::GenericTypographic(), &result);
    return result;
}
RectF AnnotationBounds(Graphics& g, const Annotation& a) {
    if (a.points.empty()) return {};
    if(a.kind==Annotation::Watermark){
        if(a.tiled)return {0,0,a.extent.Width,a.extent.Height};
        const auto box=WatermarkText(g,a);const float radians=a.angle*3.141592654f/180;
        const float w=abs(box.Width*cos(radians))+abs(box.Height*sin(radians)),h=abs(box.Width*sin(radians))+abs(box.Height*cos(radians));
        return {a.points[0].X-w/2,a.points[0].Y-h/2,w,h};
    }
    if (a.kind == Annotation::Text) {
        auto r = AnnotationTextBounds(g, a);
        if (r.Width < 1) r = {a.points[0].X, a.points[0].Y, a.size, a.size * 1.4f};
        return r;
    }
    float l = a.points[0].X, t = a.points[0].Y, r = l, b = t;
    for (auto p : a.points) { l = min(l, p.X); t = min(t, p.Y); r = max(r, p.X); b = max(b, p.Y); }
    RectF result(l, t, r - l, b - t);
    if (a.kind != Annotation::Mosaic) result.Inflate(a.kind == Annotation::Arrow ? max(10.0f, a.size * 4) : a.size / 2,
        a.kind == Annotation::Arrow ? max(10.0f, a.size * 4) : a.size / 2);
    return result;
}
bool HitAnnotation(Graphics& g, const Annotation& a, PointF p, float tolerance) {
    if (a.points.empty()) return false;
    if (a.kind == Annotation::Text || a.kind == Annotation::Mosaic || a.kind == Annotation::Watermark) {
        auto r = AnnotationBounds(g, a); r.Inflate(tolerance, tolerance); return r.Contains(p);
    }
    const float radius = tolerance + a.size / 2;
    if (a.kind == Annotation::Rectangle && a.points.size() >= 2) {
        const auto r = ShapeRect(a);
        const PointF corners[]{{r.X,r.Y},{r.GetRight(),r.Y},{r.GetRight(),r.GetBottom()},{r.X,r.GetBottom()}};
        for (int i = 0; i < 4; ++i) if (SegmentDistance(p, corners[i], corners[(i + 1) % 4]) <= radius) return true;
        return false;
    }
    if (a.kind == Annotation::Arrow) {
        if (a.points.size() > 1 && hypot(p.X - a.points.back().X, p.Y - a.points.back().Y) <= max(radius, a.size * 2)) return true;
    }
    if (a.points.size() == 1) return SegmentDistance(p, a.points[0], a.points[0]) <= radius;
    for (size_t i = 1; i < a.points.size(); ++i) if (SegmentDistance(p, a.points[i-1], a.points[i]) <= radius) return true;
    return false;
}
unique_ptr<Bitmap> BuildMosaic(Bitmap& source, int block, wstring& error) {
    block = clamp(block, 8, 32);
    const int w = static_cast<int>(source.GetWidth()), h = static_cast<int>(source.GetHeight());
    auto result = make_unique<Bitmap>((w + block - 1) / block, (h + block - 1) / block, PixelFormat32bppARGB);
    if (result->GetLastStatus() != Ok) { error = tr("screen.annotation.001"); return {}; }
    BitmapData input{}, output{}; Rect sr(0,0,w,h), dr(0,0,result->GetWidth(),result->GetHeight());
    if (source.LockBits(&sr, ImageLockModeRead, PixelFormat32bppARGB, &input) != Ok) { error = tr("screen.annotation.002"); return {}; }
    if (result->LockBits(&dr, ImageLockModeWrite, PixelFormat32bppARGB, &output) != Ok) {
        source.UnlockBits(&input); error = tr("screen.annotation.003"); return {};
    }
    for (int by = 0; by < dr.Height; ++by) {
        auto dst = reinterpret_cast<UINT*>(static_cast<BYTE*>(output.Scan0) + by * output.Stride);
        for (int bx = 0; bx < dr.Width; ++bx) {
            unsigned long long a=0,r=0,g=0,b=0,count=0;
            for (int y = by * block; y < min(h,(by+1)*block); ++y) {
                const auto row = reinterpret_cast<const UINT*>(static_cast<const BYTE*>(input.Scan0) + y * input.Stride);
                for (int x=bx*block; x<min(w,(bx+1)*block); ++x) {
                    const UINT v=row[x], alpha=v>>24; a+=alpha; r+=((v>>16)&255)*alpha;
                    g+=((v>>8)&255)*alpha; b+=(v&255)*alpha; ++count;
                }
            }
            dst[bx] = a ? static_cast<UINT>(((a/count)<<24)|((r/a)<<16)|((g/a)<<8)|(b/a)) : 0;
        }
    }
    source.UnlockBits(&input); result->UnlockBits(&output); return result;
}
void DrawMosaic(Graphics& g, Bitmap& mosaic, const Annotation& a) {
    if (a.points.size() < 2) return;
    const auto saved=g.Save(); g.SetClip(ShapeRect(a), CombineModeIntersect);
    // Replace the covered pixels, even for transparent sources: source-over
    // would let an annotation underneath show through a translucent mask.
    g.SetCompositingMode(CompositingModeSourceCopy);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor); g.SetPixelOffsetMode(PixelOffsetModeHalf);
    const float block=a.size;
    g.DrawImage(&mosaic, RectF(0,0,mosaic.GetWidth()*block,mosaic.GetHeight()*block),
        0,0,static_cast<REAL>(mosaic.GetWidth()),static_cast<REAL>(mosaic.GetHeight()),UnitPixel);
    g.Restore(saved);
}
unique_ptr<Bitmap> RenderAnnotations(Bitmap& source, const vector<Annotation>& annotations, wstring& error) {
    unique_ptr<Bitmap> result(source.Clone(0, 0, static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight()), PixelFormat32bppARGB));
    if (!result || result->GetLastStatus() != Ok) { error = tr("screen.annotation.004"); return {}; }
    Graphics g(result.get());
    if (g.GetLastStatus() != Ok) { error = tr("screen.annotation.005"); return {}; }
    g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetClip(Rect(0, 0, static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight())));
    array<unique_ptr<Bitmap>,4> mosaics;
    for (const auto& a : annotations) if (a.kind != Annotation::Mosaic) {
        DrawAnnotation(g,a);
        if (g.GetLastStatus() != Ok) { error=tr("screen.annotation.006"); return {}; }
    }
    // Privacy masks always cover other annotations, including annotations added later.
    for (const auto& a : annotations) if (a.kind == Annotation::Mosaic) {
        const int block=clamp(static_cast<int>(a.size)/8,1,4)*8, i=block/8-1;
        if (!mosaics[i]) mosaics[i]=BuildMosaic(source,block,error);
        if (!mosaics[i]) return {};
        auto mask=a; mask.size=static_cast<float>(block); DrawMosaic(g,*mosaics[i],mask);
        if (g.GetLastStatus() != Ok) { error=tr("screen.annotation.007"); return {}; }
    }
    return result;
}
