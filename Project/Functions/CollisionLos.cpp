#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CollisionLos.h"

#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

struct Vec3f {
	float x = 0.f, y = 0.f, z = 0.f;
	Vec3f() = default;
	Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
	explicit Vec3f(const Vector3& v)
		: x(static_cast<float>(v.x)), y(static_cast<float>(v.y)), z(static_cast<float>(v.z)) {}
	float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

struct DVec3 { double x = 0, y = 0, z = 0; };
struct DQuat { double w = 1, x = 0, y = 0, z = 0; };
struct FVec3d { double x = 0, y = 0, z = 0; };
struct FQuat4d { double x = 0, y = 0, z = 0, w = 1; };
struct TArrayHeader { uintptr_t data = 0; int32_t num = 0, max = 0; };
struct Triangle { Vec3f p0, p1, p2; };

inline bool IsPtrValid(uintptr_t p) { return Memory::IsValidPtrFast2(p); }
inline DVec3 ToDVec3(const FVec3d& v) { return { v.x, v.y, v.z }; }
inline DQuat ToDQuat(const FQuat4d& q) { return { q.w, q.x, q.y, q.z }; }
inline Vec3f ToVec3f(const DVec3& v) { return Vec3f(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)); }
inline DVec3 VecMin(const DVec3& a, const DVec3& b) { return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) }; }
inline DVec3 VecMax(const DVec3& a, const DVec3& b) { return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) }; }
inline Vec3f VecMinf(const Vec3f& a, const Vec3f& b) { return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) }; }
inline Vec3f VecMaxf(const Vec3f& a, const Vec3f& b) { return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) }; }
inline DVec3 Cross(const DVec3& a, const DVec3& b) {
	return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x };
}
inline DVec3 QuatRotate(const DQuat& q, const DVec3& v) {
	const DVec3 u{ q.x, q.y, q.z }; const double s = q.w;
	const DVec3 uv = Cross(u, v), uuv = Cross(u, uv);
	return { v.x + uv.x*(2*s) + uuv.x*2, v.y + uv.y*(2*s) + uuv.y*2, v.z + uv.z*(2*s) + uuv.z*2 };
}
inline DQuat QuatMultiply(const DQuat& a, const DQuat& b) {
	return {
		a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
		a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
		a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
		a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w
	};
}
inline DQuat RotatorToQuat(double pitch, double yaw, double roll) {
	constexpr double k = M_PI / 360.0;
	const double sp=std::sin(pitch*k), cp=std::cos(pitch*k), sy=std::sin(yaw*k), cy=std::cos(yaw*k);
	const double sr=std::sin(roll*k), cr=std::cos(roll*k);
	return { cr*cp*cy+sr*sp*sy, cr*sp*cy+sr*cp*sy, cr*cp*sy-sr*sp*cy, sr*cp*cy-cr*sp*sy };
}
inline double VecLength(const DVec3& v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
inline DVec3 TransformLocalToWorld(const DQuat& cr, const DVec3& ct, const DVec3& cs, const DVec3& local) {
	const DVec3 scaled{ local.x*cs.x, local.y*cs.y, local.z*cs.z };
	const DVec3 r = QuatRotate(cr, scaled);
	return { ct.x+r.x, ct.y+r.y, ct.z+r.z };
}
inline DVec3 TransformLocalToWorld2(const DQuat& cr, const DVec3& ct, const DVec3& cs, const DQuat& lr, const DVec3& local) {
	const DVec3 scaled{ local.x*cs.x, local.y*cs.y, local.z*cs.z };
	const DVec3 r = QuatRotate(QuatMultiply(cr, lr), scaled);
	return { ct.x+r.x, ct.y+r.y, ct.z+r.z };
}

uintptr_t ReadStaticMeshAsset(uintptr_t smcRoot) {
	if (!IsPtrValid(smcRoot))
		return 0;
	uintptr_t mesh = Memory::read<uintptr_t>(smcRoot + Offsets::StaticMesh);
	if (IsPtrValid(mesh))
		return mesh;
	mesh = Memory::read<uintptr_t>(smcRoot + Offsets::StaticMeshLegacy);
	return IsPtrValid(mesh) ? mesh : 0;
}

bool ComponentHasStaticMeshAsset(uintptr_t component) {
	return ReadStaticMeshAsset(component) != 0;
}

DVec3 ThinExtendedBoundsHalfForVerticalExtent(DVec3 half, const DQuat& compRot, const DVec3& compScale) {
	constexpr double kVerticalAxisScale = 0.38, kMinHalfCm = 1e-4;
	const double sx=std::fabs(compScale.x), sy=std::fabs(compScale.y), sz=std::fabs(compScale.z);
	const DVec3 wx=QuatRotate(compRot,{half.x*sx,0,0}), wy=QuatRotate(compRot,{0,half.y*sy,0}), wz=QuatRotate(compRot,{0,0,half.z*sz});
	const double vx=std::fabs(wx.z), vy=std::fabs(wy.z), vz=std::fabs(wz.z);
	if (vx>=vy && vx>=vz && vx>1e-9) half.x*=kVerticalAxisScale;
	else if (vy>=vx && vy>=vz && vy>1e-9) half.y*=kVerticalAxisScale;
	else if (vz>1e-9) half.z*=kVerticalAxisScale;
	half.x=std::max(half.x,kMinHalfCm); half.y=std::max(half.y,kMinHalfCm); half.z=std::max(half.z,kMinHalfCm);
	return half;
}
DVec3 SanitizeExtendedBoundsOrigin(DVec3 origin, const DVec3& half) {
	constexpr double kOriginVsMaxHalf = 2.35;
	const double mx = std::max({ std::fabs(half.x), std::fabs(half.y), std::fabs(half.z), 1e-6 });
	const double om = VecLength(origin);
	if (!std::isfinite(om) || om > kOriginVsMaxHalf * mx) return {0,0,0};
	return origin;
}
bool LosWorldAabbWithinCaps(DVec3 mn, DVec3 mx, double maxDiag, double maxHalf) {
	if (maxDiag<=0 && maxHalf<=0) return true;
	const DVec3 ext{ mx.x-mn.x, mx.y-mn.y, mx.z-mn.z };
	if (maxDiag>0 && VecLength(ext)>maxDiag) return false;
	if (maxHalf>0 && 0.5*std::max(ext.x,std::max(ext.y,ext.z))>maxHalf) return false;
	return true;
}
bool LosBoxPrimitiveWithinCaps(DVec3 c, DQuat br, float hx, float hy, float hz,
	const DQuat& cr, const DVec3& ct, const DVec3& cs, double maxDiag, double maxHalf) {
	const DVec3 h{hx,hy,hz};
	static constexpr DVec3 signs[8]={{-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},{-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1}};
	DVec3 mn{1e300,1e300,1e300}, mx{-1e300,-1e300,-1e300};
	for (int i=0;i<8;++i) {
		const DVec3 local{c.x+h.x*signs[i].x,c.y+h.y*signs[i].y,c.z+h.z*signs[i].z};
		const DVec3 w=TransformLocalToWorld2(cr,ct,cs,br,local);
		mn=VecMin(mn,w); mx=VecMax(mx,w);
	}
	return LosWorldAabbWithinCaps(mn,mx,maxDiag,maxHalf);
}
bool LosSpherePrimitiveWithinCaps(DVec3 c, float radius, const DQuat& cr, const DVec3& ct, const DVec3& cs, double maxDiag, double maxHalf) {
	const double r=radius;
	DVec3 mn{1e300,1e300,1e300}, mx{-1e300,-1e300,-1e300};
	const DVec3 ax[6]={{c.x+r,c.y,c.z},{c.x-r,c.y,c.z},{c.x,c.y+r,c.z},{c.x,c.y-r,c.z},{c.x,c.y,c.z+r},{c.x,c.y,c.z-r}};
	for (int i=0;i<6;++i) { const DVec3 w=TransformLocalToWorld(cr,ct,cs,ax[i]); mn=VecMin(mn,w); mx=VecMax(mx,w); }
	return LosWorldAabbWithinCaps(mn,mx,maxDiag,maxHalf);
}
void AppendBoxTriangles(const DVec3& c, const DQuat& br, float hx, float hy, float hz,
	const DQuat& cr, const DVec3& ct, const DVec3& cs, std::vector<Triangle>& out) {
	const DVec3 h{hx,hy,hz};
	static constexpr DVec3 signs[8]={{-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},{-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1}};
	Vec3f wp[8];
	for (int i=0;i<8;++i) {
		const DVec3 local{c.x+h.x*signs[i].x,c.y+h.y*signs[i].y,c.z+h.z*signs[i].z};
		wp[i]=ToVec3f(TransformLocalToWorld2(cr,ct,cs,br,local));
	}
	static constexpr int faces[12][3]={{0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},{2,6,7},{2,7,3},{0,3,7},{0,7,4},{1,5,6},{1,6,2}};
	out.reserve(out.size()+12);
	for (const auto& f:faces) out.push_back({wp[f[0]],wp[f[1]],wp[f[2]]});
}
void AppendSphereTriangles(const DVec3& c, float radius, const DQuat& cr, const DVec3& ct, const DVec3& cs, std::vector<Triangle>& out) {
	const double r=radius;
	const DVec3 lv[6]={{c.x+r,c.y,c.z},{c.x-r,c.y,c.z},{c.x,c.y+r,c.z},{c.x,c.y-r,c.z},{c.x,c.y,c.z+r},{c.x,c.y,c.z-r}};
	Vec3f wp[6]; for (int i=0;i<6;++i) wp[i]=ToVec3f(TransformLocalToWorld(cr,ct,cs,lv[i]));
	static constexpr int oct[8][3]={{0,2,4},{2,1,4},{1,3,4},{3,0,4},{0,5,2},{2,5,1},{1,5,3},{3,5,0}};
	out.reserve(out.size()+8); for (const auto& f:oct) out.push_back({wp[f[0]],wp[f[1]],wp[f[2]]});
}
void AppendSphylTriangles(const DVec3& c, const DQuat& sr, float radius, float halfLen,
	const DQuat& cr, const DVec3& ct, const DVec3& cs, std::vector<Triangle>& out) {
	AppendBoxTriangles(c,sr,radius,radius,halfLen+radius,cr,ct,cs,out);
}

bool FillConvexVerticesWorld(const TArrayHeader& vertHdr, const DQuat& cr, const DVec3& ct, const DVec3& cs, std::vector<Vec3f>& out) {
	if (!IsPtrValid(vertHdr.data) || vertHdr.num<=0 || vertHdr.num>65536) return false;
	std::vector<FVec3d> raw(static_cast<size_t>(vertHdr.num));
	if (!Memory::ReadRaw(vertHdr.data, raw.data(), static_cast<size_t>(vertHdr.num)*sizeof(FVec3d))) return false;
	out.resize(static_cast<size_t>(vertHdr.num));
	for (int32_t vi=0; vi<vertHdr.num; ++vi) {
		const DVec3 local{raw[static_cast<size_t>(vi)].x,raw[static_cast<size_t>(vi)].y,raw[static_cast<size_t>(vi)].z};
		const DVec3 w=TransformLocalToWorld(cr,ct,cs,local);
		if (!std::isfinite(w.x)||!std::isfinite(w.y)||!std::isfinite(w.z)) return false;
		out[static_cast<size_t>(vi)]=ToVec3f(w);
	}
	return true;
}
bool LoadConvexTriangleIndices(const TArrayHeader& idxHdr, int32_t vertCount, std::vector<int32_t>& out) {
	out.clear();
	if (!IsPtrValid(idxHdr.data)||idxHdr.num<=0||idxHdr.num>65536*3||idxHdr.num%3!=0||vertCount<=0) return false;
	auto countGood=[&](const std::vector<int32_t>& idx)->int {
		const int tc=static_cast<int>(idx.size()/3), scan=std::min(tc,48); int goods=0;
		for (int ti=0;ti<scan;++ti) {
			const int i0=idx[static_cast<size_t>(ti*3)], i1=idx[static_cast<size_t>(ti*3+1)], i2=idx[static_cast<size_t>(ti*3+2)];
			if (i0>=0&&i0<vertCount&&i1>=0&&i1<vertCount&&i2>=0&&i2<vertCount) ++goods;
		}
		return goods;
	};
	out.resize(static_cast<size_t>(idxHdr.num));
	if (Memory::ReadRaw(idxHdr.data,out.data(),out.size()*sizeof(int32_t)) && countGood(out)>0) return true;
	std::vector<uint16_t> u16(static_cast<size_t>(idxHdr.num));
	if (!Memory::ReadRaw(idxHdr.data,u16.data(),u16.size()*sizeof(uint16_t))) { out.clear(); return false; }
	for (size_t i=0;i<u16.size();++i) out[i]=static_cast<int32_t>(u16[i]);
	if (countGood(out)<=0) { out.clear(); return false; }
	return true;
}
void AppendBoundsTriangles(uintptr_t mesh, const DQuat& cr, const DVec3& ct, const DVec3& cs, std::vector<Triangle>& out, double maxDiag=0, double maxHalf=0) {
	if (!IsPtrValid(mesh)) return;
	const uintptr_t p=mesh+Offsets::ExtendedBounds;
	DVec3 origin=ToDVec3(Memory::read<FVec3d>(p)), half=ToDVec3(Memory::read<FVec3d>(p+0x18));
	constexpr double eps=1e-6;
	if (half.x<-eps||half.y<-eps||half.z<-eps) return;
	if (!std::isfinite(half.x)||!std::isfinite(half.y)||!std::isfinite(half.z)) return;
	if (!std::isfinite(origin.x)||!std::isfinite(origin.y)||!std::isfinite(origin.z)) return;
	if ((std::fabs(half.x)<eps&&std::fabs(half.y)<eps&&std::fabs(half.z)<eps)||std::fabs(half.x)>1e8||std::fabs(half.y)>1e8||std::fabs(half.z)>1e8) return;
	origin=SanitizeExtendedBoundsOrigin(origin,half);
	if (var::vischeck_auto_thin) half=ThinExtendedBoundsHalfForVerticalExtent(half,cr,cs);
	const double diag=std::sqrt(half.x*half.x+half.y*half.y+half.z*half.z);
	if (!std::isfinite(diag)||diag<eps) return;
	constexpr double kMaxCornerCm=18000.0;
	if (diag>kMaxCornerCm) return;
	const double sx=std::fabs(cs.x), sy=std::fabs(cs.y), sz=std::fabs(cs.z);
	const double hxw=std::fabs(half.x*sx), hyw=std::fabs(half.y*sy), hzw=std::fabs(half.z*sz);
	const double worldDiag=std::sqrt(hxw*hxw+hyw*hyw+hzw*hzw);
	if (!std::isfinite(worldDiag)||worldDiag>kMaxCornerCm) return;
	const double maxScaledHalf=std::max(hxw,std::max(hyw,hzw));
	if (maxDiag>0 && worldDiag>maxDiag) return;
	if (maxHalf>0 && maxScaledHalf>maxHalf) return;
	AppendBoxTriangles(origin,{1,0,0,0},static_cast<float>(half.x),static_cast<float>(half.y),static_cast<float>(half.z),cr,ct,cs,out);
}

std::vector<Triangle> ReadWorldTriangles(uintptr_t smc, uintptr_t meshCached=0, double maxDiag=0, double maxHalf=0) {
	std::vector<Triangle> out;
	if (!IsPtrValid(smc)) return out;
	const uintptr_t mesh=ReadStaticMeshAsset(smc);
	if (!IsPtrValid(mesh)) return out;
	const uintptr_t xform=smc+Offsets::ComponentToWorld;
	const DQuat cr=ToDQuat(Memory::read<FQuat4d>(xform));
	const DVec3 ct=ToDVec3(Memory::read<FVec3d>(xform+0x20)), cs=ToDVec3(Memory::read<FVec3d>(xform+0x40));
	const bool losCap=maxDiag>0||maxHalf>0;
	const uintptr_t bodySetup=Memory::read<uintptr_t>(mesh+Offsets::BodySetup);
	if (!IsPtrValid(bodySetup)) return out;
	const uintptr_t aggBase=bodySetup+Offsets::AggGeom;
	const TArrayHeader convArr=Memory::read<TArrayHeader>(aggBase+Offsets::AggGeom_ConvexElems);
	if (IsPtrValid(convArr.data)&&convArr.num>0&&convArr.num<=2048) {
		for (int32_t ei=0;ei<convArr.num;++ei) {
			const uintptr_t elem=convArr.data+static_cast<uintptr_t>(ei)*Offsets::ConvexElem_Stride;
			const TArrayHeader vertHdr=Memory::read<TArrayHeader>(elem+Offsets::ConvexElem_VertexData);
			const TArrayHeader idxHdr=Memory::read<TArrayHeader>(elem+Offsets::ConvexElem_IndexData);
			if (!IsPtrValid(vertHdr.data)||vertHdr.num<=0||vertHdr.num>65536) continue;
			if (!IsPtrValid(idxHdr.data)||idxHdr.num<=0||idxHdr.num>65536*3||idxHdr.num%3!=0) continue;
			std::vector<Vec3f> wv; if (!FillConvexVerticesWorld(vertHdr,cr,ct,cs,wv)) continue;
			std::vector<int32_t> rawIdx; if (!LoadConvexTriangleIndices(idxHdr,vertHdr.num,rawIdx)) continue;
			if (losCap) {
				DVec3 mn{1e300,1e300,1e300}, mx{-1e300,-1e300,-1e300};
				for (const auto& vv:wv) { const DVec3 vd{vv.x,vv.y,vv.z}; mn=VecMin(mn,vd); mx=VecMax(mx,vd); }
				if (!LosWorldAabbWithinCaps(mn,mx,maxDiag,maxHalf)) continue;
			}
			const int32_t triCount=idxHdr.num/3;
			out.reserve(out.size()+static_cast<size_t>(triCount));
			for (int32_t ti=0;ti<triCount;++ti) {
				const int32_t i0=rawIdx[static_cast<size_t>(ti*3)], i1=rawIdx[static_cast<size_t>(ti*3+1)], i2=rawIdx[static_cast<size_t>(ti*3+2)];
				if (i0<0||i0>=vertHdr.num||i1<0||i1>=vertHdr.num||i2<0||i2>=vertHdr.num) continue;
				out.push_back({wv[static_cast<size_t>(i0)],wv[static_cast<size_t>(i1)],wv[static_cast<size_t>(i2)]});
			}
		}
	}
	const TArrayHeader boxArr=Memory::read<TArrayHeader>(aggBase+Offsets::AggGeom_BoxElems);
	if (IsPtrValid(boxArr.data)&&boxArr.num>0&&boxArr.num<=512) {
		for (int32_t ei=0;ei<boxArr.num;++ei) {
			const uintptr_t elem=boxArr.data+static_cast<uintptr_t>(ei)*Offsets::BoxElem_Stride;
			const DVec3 bc=ToDVec3(Memory::read<FVec3d>(elem+Offsets::BoxElem_Center));
			const double pitch=Memory::read<double>(elem+Offsets::BoxElem_Rotation), yaw=Memory::read<double>(elem+Offsets::BoxElem_Rotation+8), roll=Memory::read<double>(elem+Offsets::BoxElem_Rotation+16);
			const float hx=Memory::read<float>(elem+Offsets::BoxElem_XExtent)*0.5f, hy=Memory::read<float>(elem+Offsets::BoxElem_YExtent)*0.5f, hz=Memory::read<float>(elem+Offsets::BoxElem_ZExtent)*0.5f;
			if (hx<=0||hy<=0||hz<=0) continue;
			const DQuat br=RotatorToQuat(pitch,yaw,roll);
			if (losCap&&!LosBoxPrimitiveWithinCaps(bc,br,hx,hy,hz,cr,ct,cs,maxDiag,maxHalf)) continue;
			AppendBoxTriangles(bc,br,hx,hy,hz,cr,ct,cs,out);
		}
	}
	const TArrayHeader sphArr=Memory::read<TArrayHeader>(aggBase+Offsets::AggGeom_SphereElems);
	if (IsPtrValid(sphArr.data)&&sphArr.num>0&&sphArr.num<=512) {
		for (int32_t ei=0;ei<sphArr.num;++ei) {
			const uintptr_t elem=sphArr.data+static_cast<uintptr_t>(ei)*Offsets::SphereElem_Stride;
			const DVec3 bc=ToDVec3(Memory::read<FVec3d>(elem+Offsets::SphereElem_Center));
			const float r=Memory::read<float>(elem+Offsets::SphereElem_Radius);
			if (r<=0) continue;
			if (losCap&&!LosSpherePrimitiveWithinCaps(bc,r,cr,ct,cs,maxDiag,maxHalf)) continue;
			AppendSphereTriangles(bc,r,cr,ct,cs,out);
		}
	}
	const TArrayHeader sphylArr=Memory::read<TArrayHeader>(aggBase+Offsets::AggGeom_SphylElems);
	if (IsPtrValid(sphylArr.data)&&sphylArr.num>0&&sphylArr.num<=512) {
		for (int32_t ei=0;ei<sphylArr.num;++ei) {
			const uintptr_t elem=sphylArr.data+static_cast<uintptr_t>(ei)*Offsets::SphylElem_Stride;
			const DVec3 bc=ToDVec3(Memory::read<FVec3d>(elem+Offsets::SphylElem_Center));
			const double pitch=Memory::read<double>(elem+Offsets::SphylElem_Rotation), yaw=Memory::read<double>(elem+Offsets::SphylElem_Rotation+8), roll=Memory::read<double>(elem+Offsets::SphylElem_Rotation+16);
			const float radius=Memory::read<float>(elem+Offsets::SphylElem_Radius), length=Memory::read<float>(elem+Offsets::SphylElem_Length);
			if (radius<=0) continue;
			const DQuat sr=RotatorToQuat(pitch,yaw,roll); const float hh=length*0.5f;
			if (losCap&&!LosBoxPrimitiveWithinCaps(bc,sr,radius,radius,hh+radius,cr,ct,cs,maxDiag,maxHalf)) continue;
			AppendSphylTriangles(bc,sr,radius,hh,cr,ct,cs,out);
		}
	}
	return out;
}

uint64_t StaticMeshInstanceFingerprint(uintptr_t smcRoot) {
	if (!IsPtrValid(smcRoot)) return 0;
	const uintptr_t mesh=ReadStaticMeshAsset(smcRoot);
	if (!IsPtrValid(mesh)) return 0;
	const uintptr_t xform=smcRoot+Offsets::ComponentToWorld;
	const FQuat4d qraw=Memory::read<FQuat4d>(xform);
	const FVec3d traw=Memory::read<FVec3d>(xform+0x20), sraw=Memory::read<FVec3d>(xform+0x40);
	double qw=qraw.w,qx=qraw.x,qy=qraw.y,qz=qraw.z;
	if (qw<0){qw=-qw;qx=-qx;qy=-qy;qz=-qz;}
	auto qi=[](double v){return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v*10000.0)));};
	auto ti=[](double v){return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v)));};
	auto si=[](double v){return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v*1000.0)));};
	uint64_t h=static_cast<uint64_t>(mesh);
	auto mix=[&](uint64_t v){h^=v+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);};
	mix(qi(qx));mix(qi(qy));mix(qi(qz));mix(qi(qw));mix(ti(traw.x));mix(ti(traw.y));mix(ti(traw.z));mix(si(sraw.x));mix(si(sraw.y));mix(si(sraw.z));
	return h;
}
struct KDTriangle { Vec3f p0,p1,p2; };
struct KDNode {
	Vec3f bbMin,bbMax; std::vector<KDTriangle> triangles; KDNode* left=nullptr; KDNode* right=nullptr;
	~KDNode(){delete left;delete right;}
};
namespace KdDetail {
KDNode* Build(std::vector<KDTriangle>& tris,int depth=0) {
	if (tris.empty()) return nullptr;
	auto* node=new KDNode(); node->bbMin=node->bbMax=tris[0].p0;
	for (const auto& t:tris) for (const auto* p:{&t.p0,&t.p1,&t.p2}) { node->bbMin=VecMinf(node->bbMin,*p); node->bbMax=VecMaxf(node->bbMax,*p); }
	if (static_cast<int>(tris.size())<=8) { node->triangles=std::move(tris); return node; }
	const int axis=depth%3;
	std::nth_element(tris.begin(),tris.begin()+tris.size()/2,tris.end(),[axis](const KDTriangle& a,const KDTriangle& b){
		return (a.p0[axis]+a.p1[axis]+a.p2[axis])/3.f < (b.p0[axis]+b.p1[axis]+b.p2[axis])/3.f;
	});
	const size_t mid=tris.size()/2;
	std::vector<KDTriangle> lt(tris.begin(),tris.begin()+static_cast<std::ptrdiff_t>(mid)), rt(tris.begin()+static_cast<std::ptrdiff_t>(mid),tris.end());
	node->left=Build(lt,depth+1); node->right=Build(rt,depth+1); return node;
}
Vec3f SafeInvDir(const Vec3f& rd) {
	constexpr float e=1e-12f;
	return {(std::fabs(rd.x)>e)?1.f/rd.x:(rd.x>=0?1e12f:-1e12f),(std::fabs(rd.y)>e)?1.f/rd.y:(rd.y>=0?1e12f:-1e12f),(std::fabs(rd.z)>e)?1.f/rd.z:(rd.z>=0?1e12f:-1e12f)};
}
bool RayAabb(const Vec3f& ro,const Vec3f& inv,const Vec3f& bmin,const Vec3f& bmax) {
	const Vec3f t0{(bmin.x-ro.x)*inv.x,(bmin.y-ro.y)*inv.y,(bmin.z-ro.z)*inv.z};
	const Vec3f t1{(bmax.x-ro.x)*inv.x,(bmax.y-ro.y)*inv.y,(bmax.z-ro.z)*inv.z};
	const Vec3f tmin{std::min(t0.x,t1.x),std::min(t0.y,t1.y),std::min(t0.z,t1.z)};
	const Vec3f tmax{std::max(t0.x,t1.x),std::max(t0.y,t1.y),std::max(t0.z,t1.z)};
	const float enter=std::max(tmin.x,std::max(tmin.y,tmin.z)), leave=std::min(tmax.x,std::min(tmax.y,tmax.z));
	return leave>=0.f && enter<=leave;
}
bool RayTriSegment(const Vec3f& ro,const Vec3f& rd,const KDTriangle& t,float tMin,float tMax) {
	constexpr float e=1e-6f;
	const Vec3f e1{t.p1.x-t.p0.x,t.p1.y-t.p0.y,t.p1.z-t.p0.z}, e2{t.p2.x-t.p0.x,t.p2.y-t.p0.y,t.p2.z-t.p0.z};
	const Vec3f h{rd.y*e2.z-rd.z*e2.y,rd.z*e2.x-rd.x*e2.z,rd.x*e2.y-rd.y*e2.x};
	const float a=e1.x*h.x+e1.y*h.y+e1.z*h.z; if (a>-e&&a<e) return false;
	const float f=1.f/a; const Vec3f s{ro.x-t.p0.x,ro.y-t.p0.y,ro.z-t.p0.z};
	const float u=f*(s.x*h.x+s.y*h.y+s.z*h.z); if (u<0||u>1) return false;
	const Vec3f q{s.y*e1.z-s.z*e1.y,s.z*e1.x-s.x*e1.z,s.x*e1.y-s.y*e1.x};
	const float v=f*(rd.x*q.x+rd.y*q.y+rd.z*q.z); if (v<0||u+v>1) return false;
	const float tv=f*(e2.x*q.x+e2.y*q.y+e2.z*q.z); return tv>tMin+e && tv<tMax-e;
}
bool Traverse(const KDNode* node,const Vec3f& ro,const Vec3f& rd,const Vec3f& inv,float tMin,float tMax) {
	if (!node||!RayAabb(ro,inv,node->bbMin,node->bbMax)) return false;
	for (const auto& tri:node->triangles) if (RayTriSegment(ro,rd,tri,tMin,tMax)) return true;
	return Traverse(node->left,ro,rd,inv,tMin,tMax)||Traverse(node->right,ro,rd,inv,tMin,tMax);
}
}
std::mutex g_mutex; KDNode* g_tree=nullptr; std::size_t g_triCount=0; std::mutex g_bgReadMutex;
constexpr double kLosPrimDiag=1600.0, kLosPrimHalf=650.0;

void Rebuild(const std::vector<uintptr_t>& smcAddrs) {
	std::vector<KDTriangle> allTris; allTris.reserve(smcAddrs.size()*64u);
	for (const uintptr_t smc:smcAddrs) {
		const auto tris=ReadWorldTriangles(smc,0,kLosPrimDiag,kLosPrimHalf);
		if (!tris.empty()) { for (const auto& t:tris) allTris.push_back({t.p0,t.p1,t.p2}); continue; }
		const uintptr_t mesh=ReadStaticMeshAsset(smc); if (!mesh) continue;
		const uintptr_t xform=smc+Offsets::ComponentToWorld;
		const DQuat cr=ToDQuat(Memory::read<FQuat4d>(xform));
		const DVec3 ct=ToDVec3(Memory::read<FVec3d>(xform+0x20)), cs=ToDVec3(Memory::read<FVec3d>(xform+0x40));
		const FVec3d extOrg=Memory::read<FVec3d>(mesh+Offsets::ExtendedBounds), extExt=Memory::read<FVec3d>(mesh+Offsets::ExtendedBounds+0x18);
		const FVec3d posExt=Memory::read<FVec3d>(mesh+Offsets::PositiveBoundsExt), negExt=Memory::read<FVec3d>(mesh+Offsets::NegativeBoundsExt);
		DVec3 half{extExt.x-(posExt.x+negExt.x)*0.5,extExt.y-(posExt.y+negExt.y)*0.5,extExt.z-(posExt.z+negExt.z)*0.5};
		DVec3 org{extOrg.x-(posExt.x-negExt.x)*0.5,extOrg.y-(posExt.y-negExt.y)*0.5,extOrg.z-(posExt.z-negExt.z)*0.5};
		constexpr double kEps=1e-4,kMaxLos=650.0,kCornerHalf=850.0,kMinWorldZ=80.0;
		if (!std::isfinite(half.x)||!std::isfinite(half.y)||!std::isfinite(half.z)) continue;
		if (half.x<kEps||half.y<kEps||half.z<kEps||half.x>kMaxLos||half.y>kMaxLos||half.z>kMaxLos) continue;
		{ const DVec3 ax{std::abs(cs.x)*half.x,std::abs(cs.y)*half.y,std::abs(cs.z)*half.z}; if (VecLength(ax)>kCornerHalf) continue; }
		{
			const DVec3 colX=QuatRotate(cr,{1,0,0}), colY=QuatRotate(cr,{0,1,0}), colZ=QuatRotate(cr,{0,0,1});
			const double wz=std::abs(colX.z)*std::abs(cs.x)*half.x+std::abs(colY.z)*std::abs(cs.y)*half.y+std::abs(colZ.z)*std::abs(cs.z)*half.z;
			if (wz<kMinWorldZ) continue;
		}
		if (var::vischeck_auto_thin) half=ThinExtendedBoundsHalfForVerticalExtent(half,cr,cs);
		std::vector<Triangle> btris; btris.reserve(12);
		AppendBoxTriangles(org,{1,0,0,0},static_cast<float>(half.x),static_cast<float>(half.y),static_cast<float>(half.z),cr,ct,cs,btris);
		for (const auto& t:btris) allTris.push_back({t.p0,t.p1,t.p2});
	}
	KDNode* newTree=allTris.empty()?nullptr:KdDetail::Build(allTris);
	const size_t count=allTris.size();
	std::lock_guard lk(g_mutex); delete g_tree; g_tree=newTree; g_triCount=count;
}
std::atomic<bool> s_rebuilding{false}; std::mutex s_stateMutex; Vector3 s_lastRebuildPos{};
std::chrono::steady_clock::time_point s_lastRebuildTime{};
std::chrono::steady_clock::time_point s_lastEmptyRetryTime{};
constexpr float kRebuildMoveThresholdSq=3000.f*3000.f; constexpr auto kRebuildForceInterval=std::chrono::seconds(20);
constexpr auto kEmptyRetryInterval=std::chrono::seconds(2);
std::atomic<std::size_t> g_lastSmcCount{0};
constexpr float kCollDistSq=60000.f*60000.f;

void CollectLevelStaticMeshes(
	uintptr_t level,
	const Vector3& localPos,
	std::unordered_set<uint64_t>& seenFp,
	std::vector<uintptr_t>& out)
{
	if (!IsPtrValid(level))
		return;
	const uintptr_t actorsData=Memory::read<uintptr_t>(level+Offsets::AActors);
	const int32_t actorCount=Memory::read<int32_t>(level+Offsets::ActorsCount);
	if (!IsPtrValid(actorsData)||actorCount<=0||actorCount>65536)
		return;
	std::vector<uintptr_t> actorPtrs(static_cast<size_t>(actorCount));
	if (!Memory::ReadRaw(actorsData,actorPtrs.data(),actorPtrs.size()*sizeof(uintptr_t)))
		return;
	for (const uintptr_t actor:actorPtrs) {
		if (!IsPtrValid(actor))
			continue;
		const uintptr_t root=Memory::read<uintptr_t>(actor+Offsets::RootComponent);
		if (!IsPtrValid(root)||!ComponentHasStaticMeshAsset(root))
			continue;
		const FVec3d rawPos=Memory::read<FVec3d>(root+Offsets::WorldLocation);
		if (rawPos.x==0.0&&rawPos.y==0.0&&rawPos.z==0.0)
			continue;
		const Vector3 p(rawPos.x,rawPos.y,rawPos.z), d=p-localPos;
		const double distSq=d.x*d.x+d.y*d.y+d.z*d.z;
		if (distSq>static_cast<double>(kCollDistSq))
			continue;
		const uint64_t fp=StaticMeshInstanceFingerprint(root);
		if (fp==0||!seenFp.insert(fp).second)
			continue;
		out.push_back(root);
	}
}

void CollectStaticMeshRoots(uintptr_t uworld,const Vector3& localPos,std::vector<uintptr_t>& out) {
	std::unordered_set<uint64_t> seenFp; seenFp.reserve(4096);
	const uintptr_t persist=Memory::read<uintptr_t>(uworld+Offsets::PersistentLevel);
	CollectLevelStaticMeshes(persist, localPos, seenFp, out);
	const TArrayHeader lvlArr=Memory::read<TArrayHeader>(uworld+Offsets::Levels);
	if (!IsPtrValid(lvlArr.data)||lvlArr.num<=0||lvlArr.num>512)
		return;
	std::vector<uintptr_t> levelPtrs(static_cast<size_t>(lvlArr.num));
	if (!Memory::ReadRaw(lvlArr.data,levelPtrs.data(),levelPtrs.size()*sizeof(uintptr_t)))
		return;
	for (const uintptr_t level:levelPtrs)
		CollectLevelStaticMeshes(level, localPos, seenFp, out);
}
} // namespace

namespace CollisionLos {
bool IsVisible(const Vector3& from,const Vector3& to) {
	std::lock_guard lk(g_mutex);
	if (!g_tree) return true;
	const Vec3f ro(from), rd(static_cast<float>(to.x-from.x),static_cast<float>(to.y-from.y),static_cast<float>(to.z-from.z));
	const float len2=rd.x*rd.x+rd.y*rd.y+rd.z*rd.z; if (len2<1e-8f) return true;
	const Vec3f inv=KdDetail::SafeInvDir(rd);
	// Ignore hits very near camera (self) and at target (feet clutter).
	constexpr float kLosNear = 0.01f, kLosFar = 0.99f;
	return !KdDetail::Traverse(g_tree,ro,rd,inv,kLosNear,kLosFar);
}
void ScheduleWorldRebuild(uintptr_t uworld,const Vector3& localPos) {
	if (!IsPtrValid(uworld)||s_rebuilding.load(std::memory_order_acquire)) return;
	const std::size_t triCount=TriangleCount();
	bool moved=false, forced=false, retryEmpty=false;
	const auto now=std::chrono::steady_clock::now();
	{ std::lock_guard lk(s_stateMutex);
		const Vector3 delta=localPos-s_lastRebuildPos;
		const double dSq=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
		moved=dSq>static_cast<double>(kRebuildMoveThresholdSq);
		forced=(now-s_lastRebuildTime)>=kRebuildForceInterval;
		retryEmpty=triCount==0 && (now-s_lastEmptyRetryTime)>=kEmptyRetryInterval;
	}
	if (!moved&&!forced&&!retryEmpty) return;
	if (s_rebuilding.exchange(true)) return;
	const uintptr_t uw=uworld; const Vector3 lp=localPos;
	std::thread([uw,lp]{
		try {
			std::lock_guard readLk(g_bgReadMutex);
			std::vector<uintptr_t> smc; smc.reserve(2048);
			CollectStaticMeshRoots(uw,lp,smc);
			g_lastSmcCount.store(smc.size(), std::memory_order_release);
			Rebuild(smc);
			{ std::lock_guard lk(s_stateMutex); s_lastRebuildPos=lp; s_lastRebuildTime=std::chrono::steady_clock::now();
				if (smc.empty()) s_lastEmptyRetryTime=std::chrono::steady_clock::now(); }
		} catch (...) {}
		s_rebuilding.store(false,std::memory_order_release);
	}).detach();
}
std::size_t TriangleCount() { std::lock_guard lk(g_mutex); return g_triCount; }
std::size_t LastSmcCount() { return g_lastSmcCount.load(std::memory_order_acquire); }
bool IsRebuilding() { return s_rebuilding.load(std::memory_order_acquire); }
} // namespace CollisionLos