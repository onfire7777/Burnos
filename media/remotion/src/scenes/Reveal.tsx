import {Interactive,useCurrentFrame,interpolate} from 'remotion';
import {C,Orb,Stage,Tag,enter,clamp} from '../design';
import {sceneSchema,type SceneProps} from './types';
const RevealInner=({title,subtitle,style}:SceneProps)=>{
 const f=useCurrentFrame();const p=enter(f,20,50);const scan=interpolate(f,[0,120],[0,1],clamp);
 return <Stage duration={12} chapter="01 / MEET BURNOS" label="ALEXA+ TRACK · PRODUCT REVEAL" style={style}>
  <div style={{position:'absolute',left:100,top:230,width:990,zIndex:2}}>
   <Tag style={{opacity:enter(f,5)}}>A little presence. A lot of possibility.</Tag>
   <div style={{fontSize:111,lineHeight:1.02,letterSpacing:-6,fontWeight:600,whiteSpace:'pre-line',marginTop:40,opacity:p,transform:`translateY(${(1-p)*60}px)`}}>{title}</div>
   <div style={{fontSize:33,color:C.muted,lineHeight:1.5,marginTop:38,maxWidth:790,opacity:enter(f,90)}}>{subtitle}</div>
   <div style={{display:'flex',gap:14,marginTop:45,opacity:enter(f,170)}}><Tag>Voice</Tag><Tag color={C.violet}>Memory</Tag><Tag color={C.green}>Action</Tag></div>
  </div>
  <div style={{position:'absolute',left:1105,top:200,transform:`scale(${.65+p*.35}) rotate(${(1-p)*-35}deg)`,opacity:p}}><Orb size={610}/></div>
  {[0,1,2,3].map(i=><div key={i} style={{position:'absolute',left:1075+i*25,top:190+i*27,width:550,height:550,border:'1px solid #5ad2ff25',borderRadius:'50%',transform:`scale(${1+scan*.45+i*.08})`,opacity:(1-scan)*.8}}/>)}
  <div style={{position:'absolute',left:1280,top:800,fontSize:23,color:C.cyan,letterSpacing:7,opacity:enter(f,180)}}>READY WHEN YOU ARE</div>
 </Stage>;
};
export const Reveal=Interactive.withSchema({Component:RevealInner,componentName:'<Reveal>',schema:sceneSchema,wrapInSequence:true});
