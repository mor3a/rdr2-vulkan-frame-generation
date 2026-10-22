#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace fgvk::motion {
struct Sample { double x,y,depth,mx,my; };
struct Fit {
 double matrix[4][4]{},rotation[3][3]{},translationCoefficient[3]{};
 double projectionX=0,projectionY=0,rigidity=0;
 double determinant=0;
 const char* failure="insufficient samples";
 double medianPixels=0,inliers=0; bool valid=false;
};
inline bool Solve(double a[11][12],unsigned n,double* out){
 for(unsigned c=0;c<n;++c){unsigned pivot=c;for(unsigned r=c+1;r<n;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
  if(!std::isfinite(a[pivot][c]) || std::abs(a[pivot][c])<1e-12)return false;
  if(pivot!=c)for(unsigned j=0;j<=n;++j)std::swap(a[pivot][j],a[c][j]);
  double divisor=a[c][c];for(unsigned j=c;j<=n;++j)a[c][j]/=divisor;
  for(unsigned r=0;r<n;++r)if(r!=c){double f=a[r][c];for(unsigned j=c;j<=n;++j)a[r][j]-=f*a[c][j];}
 }
 for(unsigned i=0;i<n;++i){out[i]=a[i][n];if(!std::isfinite(out[i]))return false;}return true;
}
inline double Error(const Sample& s,const double* h,double depthScale,double width,double height){
 const double d=s.depth*depthScale,den=h[8]*s.x+h[9]*s.y+h[10]*d+1.;
 if(std::abs(den)<1e-8)return 1e8;
 const double x=(h[0]*s.x+h[1]*s.y+h[2]*d+h[3])/den;
 const double y=(h[4]*s.x+h[5]*s.y+h[6]*d+h[7])/den;
 return std::hypot((x-s.x-s.mx)*width*.5,(y-s.y-s.my)*height*.5);
}
inline double Median(std::vector<double>& v){auto p=v.begin()+v.size()/2;std::nth_element(v.begin(),p,v.end());return *p;}
inline bool Estimate(const std::vector<Sample>& samples,const std::vector<unsigned>& selected,const double* seed,double depthScale,double width,double height,double* out){
 double a[11][12]{};
 for(unsigned index:selected){const auto& s=samples[index];const double p[]={s.x,s.y,s.depth*depthScale,1.};const double targets[]={s.x+s.mx,s.y+s.my};
  const double error=seed?Error(s,seed,depthScale,width,height):0;
  const double weight=seed?std::min(1.,1.5/std::max(error,.01)):1.;
  for(unsigned k=0;k<2;++k){double row[11]{};for(unsigned j=0;j<4;++j)row[4*k+j]=p[j];for(unsigned j=0;j<3;++j)row[8+j]=-targets[k]*p[j];
   for(unsigned i=0;i<11;++i){for(unsigned j=0;j<11;++j)a[i][j]+=weight*row[i]*row[j];a[i][11]+=weight*row[i]*targets[k];}
  }
 }
 double full[11][12]{};for(unsigned i=0;i<11;++i)for(unsigned j=0;j<12;++j)full[i][j]=a[i][j];
 if(Solve(full,11,out))return true;
 // Sky/infinite depth and constant-depth planes do not constrain translation.
 // Solve the observable image homography instead of rejecting camera rotation.
 constexpr unsigned active[]={0,1,3,4,5,7,8,9};double reduced[11][12]{};
 for(unsigned i=0;i<8;++i){for(unsigned j=0;j<8;++j)reduced[i][j]=a[active[i]][active[j]];reduced[i][8]=a[active[i]][11];}
 double solved[11]{};if(!Solve(reduced,8,solved))return false;
 std::fill(out,out+11,0.);for(unsigned i=0;i<8;++i)out[active[i]]=solved[i];return true;
}
struct CameraModel { double r[3][3]{},t[3]{},f=1,previousF=1,aspect=1,a=0; };
inline bool Project(const CameraModel& c,const Sample& s,double& x,double& y){
 const double p[]={s.x*c.aspect/c.f,s.y/c.f,1.};double q[3]{};
 for(unsigned i=0;i<3;++i){q[i]=c.t[i]*(s.depth-c.a);for(unsigned j=0;j<3;++j)q[i]+=c.r[i][j]*p[j];}
 if(!std::isfinite(q[2])||std::abs(q[2])<1e-8)return false;
 x=c.previousF/c.aspect*q[0]/q[2];y=c.previousF*q[1]/q[2];return std::isfinite(x)&&std::isfinite(y);
}
inline void Rotate(CameraModel& c,const double* w){
 const double angle=std::sqrt(w[0]*w[0]+w[1]*w[1]+w[2]*w[2]);
 const double sn=angle>1e-8?std::sin(angle)/angle:1.;
 const double cs=angle>1e-8?(1.-std::cos(angle))/(angle*angle):.5;
 const double k[3][3]={{0,-w[2],w[1]},{w[2],0,-w[0]},{-w[1],w[0],0}};
 double delta[3][3]{},next[3][3]{};
 for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){delta[i][j]=(i==j)+sn*k[i][j];for(unsigned z=0;z<3;++z)delta[i][j]+=cs*k[i][z]*k[z][j];}
 for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)for(unsigned z=0;z<3;++z)next[i][j]+=delta[i][z]*c.r[z][j];
 for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)c.r[i][j]=next[i][j];
}
inline bool Orthonormalize(CameraModel& c){
 auto normalize=[&](unsigned col){double n=0;for(unsigned i=0;i<3;++i)n+=c.r[i][col]*c.r[i][col];n=std::sqrt(n);if(n<1e-8)return false;for(unsigned i=0;i<3;++i)c.r[i][col]/=n;return true;};
 if(!normalize(0))return false;double dot=0;for(unsigned i=0;i<3;++i)dot+=c.r[i][0]*c.r[i][1];for(unsigned i=0;i<3;++i)c.r[i][1]-=dot*c.r[i][0];if(!normalize(1))return false;
 for(unsigned i=0;i<3;++i)c.r[i][2]=c.r[(i+1)%3][0]*c.r[(i+2)%3][1]-c.r[(i+2)%3][0]*c.r[(i+1)%3][1];return true;
}
inline double CameraCost(const CameraModel& c,const std::vector<Sample>& samples,double width,double height){
 double cost=0;for(const auto& s:samples){double x,y;if(!Project(c,s,x,y)){cost+=1e6;continue;}double e=std::hypot((x-s.x-s.mx)*width*.5,(y-s.y-s.my)*height*.5);cost+=e<=1.5?.5*e*e:1.5*(e-.75);}return cost;
}
// The unconstrained projective fit is only an initializer. Refine one rigid
// camera model and derive ALL published matrices and pose from that model.
inline bool Refine(CameraModel& c,const std::vector<Sample>& samples,double width,double height){
 if(!Orthonormalize(c))return false;
 double lambda=1e-3,cost=CameraCost(c,samples,width,height);
 for(unsigned iteration=0;iteration<12;++iteration){
  double normal[11][12]{};
  for(const auto& s:samples){double x,y;if(!Project(c,s,x,y))continue;
   const double residual[]={(x-s.x-s.mx)*width*.5,(y-s.y-s.my)*height*.5};
   const double weight=std::min(1.,1.5/std::max(.01,std::hypot(residual[0],residual[1])));
   double jac[2][7]{};
   for(unsigned p=0;p<7;++p){CameraModel perturbed=c;const double step=p<3?1e-5:p<6?1e-4*std::max(1.,std::abs(c.t[p-3])):1e-5;
    if(p<3){double w[3]{};w[p]=step;Rotate(perturbed,w);}else if(p<6)perturbed.t[p-3]+=step;else perturbed.f*=std::exp(step);
    double px,py;if(!Project(perturbed,s,px,py))continue;jac[0][p]=(px-x)*width*.5/step;jac[1][p]=(py-y)*height*.5/step;
   }
   for(unsigned p=0;p<7;++p){for(unsigned q=0;q<7;++q)normal[p][q]+=weight*(jac[0][p]*jac[0][q]+jac[1][p]*jac[1][q]);normal[p][7]-=weight*(jac[0][p]*residual[0]+jac[1][p]*residual[1]);}
  }
  for(unsigned p=0;p<7;++p)normal[p][p]+=lambda*std::max(1.,normal[p][p]);
  double delta[11]{};if(!Solve(normal,7,delta))return false;
  CameraModel next=c;Rotate(next,delta);for(unsigned p=0;p<3;++p)next.t[p]+=delta[p+3];next.f*=std::exp(std::clamp(delta[6],-.2,.2));
  const double nextCost=next.f>.1763&&next.f<5.6713?CameraCost(next,samples,width,height):1e30;
  if(nextCost<cost){const double improvement=cost-nextCost;c=next;cost=nextCost;lambda=std::max(1e-7,lambda*.3);if(improvement<1e-5)break;}else lambda*=10.;
 }
 return Orthonormalize(c);
}
// Fit the projective mapping of this frame's depth and dense motion vectors.
// Moving objects are outliers, not a source of the camera transform. The fit is
// evaluated in pixels and must also describe a rigid camera rotation.
inline Fit Reconstruct(const std::vector<Sample>& samples,double width,double height,double projectionX,double projectionY,double projectionA,double previousProjectionY=0){
 Fit result{};if(samples.size()<32 || width<=0 || height<=0)return result;
 double maxDepth=0;for(const auto& s:samples)maxDepth=std::max(maxDepth,std::abs(s.depth));
 const double depthScale=maxDepth>1e-12?1./maxDepth:1.;
 std::vector<unsigned> all(samples.size());for(unsigned i=0;i<all.size();++i)all[i]=i;
 double best[11]{},bestError=1e20;std::vector<double> errors;errors.reserve(samples.size());uint32_t rng=0x6d2b79f5;
 for(unsigned trial=0;trial<25;++trial){std::vector<unsigned> subset;
  if(trial==0)subset=all;else for(unsigned i=0;i<18;++i){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;subset.push_back(rng%samples.size());}
  double h[11]{};if(!Estimate(samples,subset,nullptr,depthScale,width,height,h))continue;
  errors.clear();for(const auto& s:samples)errors.push_back(Error(s,h,depthScale,width,height));double median=Median(errors);
  if(median<bestError){bestError=median;std::copy(h,h+11,best);}
 }
 result.failure="projective solve singular";if(bestError==1e20)return result;
 for(unsigned iteration=0;iteration<5;++iteration){double h[11]{};if(!Estimate(samples,all,best,depthScale,width,height,h))break;std::copy(h,h+11,best);}
 errors.clear();unsigned inliers=0;for(const auto& s:samples){double e=Error(s,best,depthScale,width,height);errors.push_back(e);if(e<2.)++inliers;}
 result.medianPixels=Median(errors);result.inliers=double(inliers)/samples.size();
 result.failure="pixel residual or inlier coverage";if(result.medianPixels>2. || result.inliers<.55)return result;
 double h[3][4]{{best[0],best[1],best[2]*depthScale,best[3]}, {best[4],best[5],best[6]*depthScale,best[7]}, {best[8],best[9],best[10]*depthScale,1.}};
 // Remove the depth-dependent translation to recover Kprev * R * inverse(K).
 double rot[3][3]{};for(unsigned i=0;i<3;++i){rot[i][0]=h[i][0];rot[i][1]=h[i][1];rot[i][2]=h[i][3]+projectionA*h[i][2];}
 double det=rot[0][0]*(rot[1][1]*rot[2][2]-rot[1][2]*rot[2][1])-rot[0][1]*(rot[1][0]*rot[2][2]-rot[1][2]*rot[2][0])+rot[0][2]*(rot[1][0]*rot[2][1]-rot[1][1]*rot[2][0]);
 result.determinant=det;result.failure="singular rotation homography";
 // The fitted homogeneous denominator is fixed to +1, not to physical
 // forward depth. Its scale can be NEGATIVE for a valid camera rotation.
 // Rejecting every negative determinant incorrectly rejects fast turns.
 if(!std::isfinite(det)||std::abs(det)<=1e-8||projectionX<=0||projectionY<=0)return result;
 // Kprev * R * inverse(Kcurrent) is not a pure rotation when FOV changes.
 // Recover the two focal lengths before normalizing the projective scale.
 // Orthogonality of distinct columns determines 1/fprev^2; at zero rotation
 // it is unobservable, so retain the preceding accepted focal length.
 const double aspect=projectionY/projectionX;
 double aa=0,ab=0;
 for(unsigned i=0;i<3;++i)for(unsigned j=i+1;j<3;++j){
  const double a=aspect*aspect*rot[0][i]*rot[0][j]+rot[1][i]*rot[1][j];
  const double b=rot[2][i]*rot[2][j];aa+=a*a;ab+=a*b;
 }
 double previousF=previousProjectionY>0?previousProjectionY:projectionY;
 const double inverseF2=aa>1e-8?-ab/aa:0;
 // Once published, the previous camera is immutable. Re-estimating its focal
 // length here makes this transform disagree with the actual previous frame
 // supplied to DLSS-G, even if the motion-vector residual is very small.
 if(previousProjectionY<=0 && inverseF2>0){const double estimated=1./std::sqrt(inverseF2);if(estimated>.1763 && estimated<5.6713)previousF=estimated;}
 const double kp[]={previousF/aspect,previousF,1.};
 double lengths[3]{};
 for(unsigned j=0;j<3;++j){for(unsigned i=0;i<3;++i)lengths[j]+=std::pow(rot[i][j]/kp[i],2);lengths[j]=std::sqrt(lengths[j]);if(lengths[j]<1e-8)return result;}
 const double scale=std::copysign(lengths[2],det);
 const double focalY=std::sqrt((std::abs(scale)/lengths[0]*aspect)*(std::abs(scale)/lengths[1]));
 result.failure="focal decomposition outside physical range";
 if(!std::isfinite(focalY)||focalY<=.1763||focalY>=5.6713)return result;
 const double k[]={focalY/aspect,focalY,1.};
 double r[3][3]{};for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)r[i][j]=rot[i][j]*k[j]/(scale*kp[i]);
 double rigidity=0;for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){double value=0;for(unsigned z=0;z<3;++z)value+=r[i][z]*r[j][z];rigidity+=(value-(i==j))*(value-(i==j));}
 result.failure="rigid camera refinement";result.rigidity=rigidity;
 CameraModel model{};model.f=focalY;model.previousF=previousF;model.aspect=aspect;model.a=projectionA;
 for(unsigned i=0;i<3;++i){model.t[i]=h[i][2]/(scale*kp[i]);for(unsigned j=0;j<3;++j)model.r[i][j]=r[i][j];}
 if(!Refine(model,samples,width,height))return result;
 errors.clear();inliers=0;
 for(const auto& s:samples){double x,y;double e=Project(model,s,x,y)?std::hypot((x-s.x-s.mx)*width*.5,(y-s.y-s.my)*height*.5):1e8;errors.push_back(e);if(e<2.)++inliers;}
 result.medianPixels=Median(errors);result.inliers=double(inliers)/samples.size();
 result.failure="rigid reprojection residual";if(result.medianPixels>2.||result.inliers<.55)return result;
 result.projectionX=model.f/aspect;result.projectionY=model.f;
 for(unsigned i=0;i<3;++i){result.translationCoefficient[i]=model.t[i];for(unsigned j=0;j<3;++j)result.rotation[i][j]=model.r[i][j];}
 // Replace the projective initializer with the refined camera transform.
 for(unsigned i=0;i<3;++i){h[i][0]=kp[i]*model.r[i][0]/result.projectionX;h[i][1]=kp[i]*model.r[i][1]/result.projectionY;h[i][2]=kp[i]*model.t[i];h[i][3]=kp[i]*(model.r[i][2]-projectionA*model.t[i]);}
 // Row-vector clip coordinates; both projection matrices have the same depth
 // convention. z_prev = A*w_prev + z_current - A*w_current.
 for(unsigned row=0;row<4;++row){result.matrix[row][0]=h[0][row];result.matrix[row][1]=h[1][row];result.matrix[row][3]=h[2][row];result.matrix[row][2]=projectionA*result.matrix[row][3]+(row==2?1.:0.)-(row==3?projectionA:0.);}
 result.failure="none";result.valid=true;return result;
}
}
