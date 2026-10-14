import React from 'react';
import {AbsoluteFill, Easing, interpolate, useCurrentFrame, useVideoConfig, Interactive, type InteractivitySchema} from 'remotion';

export const C={bg:'#060913',panel:'#0d1424',text:'#f3f7ff',muted:'#99aac4',cyan:'#5ad2ff',violet:'#a791ff',green:'#60eca8',amber:'#ffc468',line:'#263550'};
export const font='Segoe UI, Arial, sans-serif';
export const ease=Easing.bezier(.16,1,.3,1);
export const clamp={extrapolateLeft:'clamp',extrapolateRight:'clamp'} as const;
export function enter(f:number, at=0, length=24){return interpolate(f,[at,at+length],[0,1],{...clamp,easing:ease});}
export function useSeconds(){const f=useCurrentFrame();const {fps}=useVideoConfig();return f/fps;}

export const Icon:React.FC<{name?:string;size?:number;color?:string}>=({name='spark',size=32,color=C.cyan})=>{
 const paths:Record<string,React.ReactNode>={
  spark:<path d="m12 2 2.5 7.5L22 12l-7.5 2.5L12 22l-2.5-7.5L2 12l7.5-2.5Z"/>,
  mic:<><rect x="9" y="2" width="6" height="13" rx="3"/><path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3M8 22h8"/></>,
  memory:<><rect x="5" y="5" width="14" height="14" rx="3"/><path d="M9 1v4m6-4v4M9 19v4m6-4v4M1 9h4m-4 6h4m14-6h4m-4 6h4M9 9h6v6H9z"/></>,
  web:<><circle cx="12" cy="12" r="10"/><ellipse cx="12" cy="12" rx="4" ry="10"/><path d="M2 12h20M4 6h16M4 18h16"/></>,
  check:<path d="m4 12 5 5L20 6"/>,
  arrow:<path d="M3 12h18m-7-7 7 7-7 7"/>,
  file:<><path d="M6 2h8l4 4v16H6zM14 2v5h4M9 11h6m-6 4h6"/></>,
  app:<><rect x="2" y="2" width="8" height="8" rx="2"/><rect x="14" y="2" width="8" height="8" rx="2"/><rect x="2" y="14" width="8" height="8" rx="2"/><rect x="14" y="14" width="8" height="8" rx="2"/></>,
  lock:<><rect x="5" y="10" width="14" height="12" rx="3"/><path d="M8 10V6a4 4 0 0 1 8 0v4M12 15v3"/></>,
  code:<path d="m8 6-6 6 6 6m8-12 6 6-6 6m-3-16-2 20"/>,
  stop:<rect x="5" y="5" width="14" height="14" rx="3"/>,
  wave:<path d="M2 10v4m4-8v12m4-16v20m4-17v14m4-12v10m4-7v4"/>,
 };
 return <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke={color} strokeWidth="1.65" strokeLinecap="round" strokeLinejoin="round">{paths[name]??paths.spark}</svg>;
};

export const Orb:React.FC<{size?:number;color?:string;energy?:number;style?:React.CSSProperties}>=({size=320,color=C.cyan,energy=1,style})=>{
 const f=useCurrentFrame(); const {fps}=useVideoConfig();const t=f/fps;
 const pulse=1+Math.sin(t*2.5)*.023*energy;
 const muted=color==='#bdc9db';const stopped=color==='#ff5454';const green=color===C.green;const amber=color===C.amber;const speaking=color==='#1c68ff';
 const middle=muted?'#8c9aac':stopped?'#ed3659':green?'#26bca0':amber?'#dc9650':speaking?'#2744b0':'#5483f8';
 const shade=muted?'#526174':stopped?'#8d183d':green?'#157069':amber?'#996037':'#443c95';
 const deep=muted?'#253144':stopped?'#3f1026':green?'#103938':amber?'#37251a':'#091b3b';
 const ring=muted?'#c7d2e0':stopped?'#ff7d94':green?'#a2ffd2':amber?'#ffdd9f':C.violet;
 return <div style={{width:size,height:size,position:'relative',flexShrink:0,...style}}>
  <div style={{position:'absolute',inset:'-25%',background:`radial-gradient(circle,${color}19 0%,${C.violet}0c 35%,transparent 65%)`,transform:`scale(${pulse})`}}/>
  <div style={{position:'absolute',inset:'12%',borderRadius:'50%',background:`radial-gradient(circle at 38% 28%,#dcf4ff 0%,${color} 9%,${middle} 24%,${shade} 46%,${deep} 68%,#020818 80%)`,boxShadow:`inset -15px -22px 35px #040616, inset 8px 12px 28px #bdeaff66, 0 0 48px ${color}33`,transform:`scale(${pulse})`}}/>
  {[0,1,2].map(i=><div key={i} style={{position:'absolute',inset:`${8+i*5}%`,borderRadius:'50%',border:`${1.5+i*.4}px solid ${i===1?ring:color}80`,boxShadow:`0 0 12px ${color}40`,transform:`rotate(${t*(10+i*4)+i*53}deg) rotateX(${48+i*15}deg) rotateY(20deg)`}}/>)}
  <div style={{position:'absolute',inset:'31%',borderRadius:'50%',background:`radial-gradient(circle,#fff 0%,#dcf7ff 12%,${color}99 42%,transparent 71%)`,filter:'blur(3px)',opacity:.65+.2*Math.sin(t*3)}}/>
 </div>;
};

export const Tag:React.FC<{children:React.ReactNode;color?:string;style?:React.CSSProperties}>=({children,color=C.cyan,style})=><div style={{display:'inline-flex',alignItems:'center',gap:10,padding:'10px 17px',fontSize:21,fontWeight:600,letterSpacing:1.3,color,border:`1px solid ${color}44`,borderRadius:100,background:`${color}0a`,...style}}>{children}</div>;

export const Card:React.FC<{children:React.ReactNode;style?:React.CSSProperties;accent?:string;at?:number}>=({children,style,accent,at=0})=>{
 const f=useCurrentFrame();const p=enter(f,at);
 return <div style={{border:`1px solid ${accent?accent+'66':C.line}`,borderRadius:24,background:'linear-gradient(145deg,#131e33f5,#0b1221fa)',boxShadow:'0 18px 50px #00000025',padding:30,opacity:p,transform:`translateY(${(1-p)*35}px)`,...style}}>{children}</div>;
};

export const Lines:React.FC<{widths?:number[];color?:string}>=({widths=[100,90,73],color='#334664'})=><div style={{display:'flex',flexDirection:'column',gap:16}}>{widths.map((w,i)=><div key={i} style={{height:8,width:`${w}%`,background:color,borderRadius:5}}/>)}</div>;

export const Wave:React.FC<{color?:string;bars?:number;height?:number}>=({color=C.cyan,bars=34,height=66})=>{
 const f=useCurrentFrame();return <div style={{height,display:'flex',alignItems:'center',justifyContent:'center',gap:5}}>{Array.from({length:bars},(_,i)=><div key={i} style={{width:5,borderRadius:6,background:color,opacity:.55+.45*Math.sin(i*.4)**2,height:8+Math.abs(Math.sin(f*.14+i*.62)*Math.sin(i*.36+f*.028))*(height-8)}}/>)}</div>;
};

const headingSchema={title:{type:'text-content',default:'In context.',description:'Headline'},subtitle:{type:'text-content',default:'',description:'Supporting line'}} as const satisfies InteractivitySchema;
export const Heading=Interactive.withSchema({componentName:'<Heading>',wrapInSequence:true,schema:headingSchema,Component:({title,subtitle,style}:{title:string;subtitle:string;style?:React.CSSProperties})=>{
 const p=enter(useCurrentFrame(),0,27);
 return <Interactive.Div style={{position:'absolute',left:90,top:160,width:1730,fontFamily:font,...style}}><div style={{fontSize:78,fontWeight:600,letterSpacing:-3.5,lineHeight:1.07,color:C.text,opacity:p,transform:`translateY(${(1-p)*24}px)`}}>{title}</div>{subtitle&&<div style={{fontSize:29,lineHeight:1.5,color:C.muted,marginTop:18,opacity:p}}>{subtitle}</div>}</Interactive.Div>;
}});

export const Stage:React.FC<{children:React.ReactNode;chapter:string;label?:string;duration:number;style?:React.CSSProperties}>=({children,chapter,label='ILLUSTRATED WORKFLOW',duration,style})=>{
 const f=useCurrentFrame(); const {fps}=useVideoConfig();const p=enter(f,0,20);const out=interpolate(f,[duration*fps-12,duration*fps],[1,0],clamp);
 return <AbsoluteFill style={{background:C.bg,fontFamily:font,color:C.text,overflow:'hidden',...style}}>
  <div style={{position:'absolute',inset:0,background:'radial-gradient(ellipse at 80% 30%,#17224077,transparent 55%),radial-gradient(ellipse at 8% 90%,#26183a55,transparent 45%)'}}/>
  <svg style={{position:'absolute',inset:0,opacity:.24}} width="1920" height="1080"><defs><pattern id="grid" width="80" height="80" patternUnits="userSpaceOnUse"><path d="M80 0H0V80" fill="none" stroke="#435679" strokeWidth=".7"/></pattern></defs><rect width="1920" height="1080" fill="url(#grid)"/></svg>
  <div style={{position:'absolute',left:90,top:52,display:'flex',alignItems:'center',gap:16}}><div style={{height:20,width:20,borderRadius:99,background:C.cyan,boxShadow:`0 0 25px ${C.cyan}88`}}/><span style={{fontSize:33,fontWeight:600,letterSpacing:-1}}>burnos</span><span style={{marginLeft:20,fontSize:17,color:C.muted,letterSpacing:2.2}}>IN CONTEXT. IN MOTION.</span></div>
  <div style={{position:'absolute',right:90,top:63,color:C.muted,fontSize:18,letterSpacing:1.5}}>{label}</div>
  {chapter.match(/^0[2-9]/)&&<div style={{position:'absolute',left:715,top:65,fontSize:17,letterSpacing:2,color:C.violet}}>DEMO PROJECT / AURORA WORKSHOP</div>}
  <div style={{position:'absolute',inset:0,opacity:p*out,transform:`translateX(${(1-p)*45-(1-out)*30}px)`}}>{children}</div>
  <div style={{position:'absolute',left:90,right:90,bottom:112,height:1,background:C.line}}/>
  <div style={{position:'absolute',bottom:77,left:90,color:C.muted,fontSize:18,letterSpacing:2}}>{chapter}</div>
  <div style={{position:'absolute',bottom:77,right:90,color:C.muted,fontSize:18,letterSpacing:1.5}}>BURNOS.APP</div>
  <div style={{position:'absolute',height:4,left:0,bottom:0,width:`${100* Math.min(1,f/(duration*fps))}%`,background:`linear-gradient(90deg,${C.violet},${C.cyan})`}}/>
  <div style={{position:'absolute',top:0,bottom:0,width:4,background:C.cyan,opacity:(1-p)*.8,left:interpolate(p,[0,1],[0,1920]),boxShadow:`0 0 90px 50px ${C.cyan}18`}}/>
 </AbsoluteFill>;
};

export const Bullet:React.FC<{icon:string;title:string;detail?:string;color?:string}>=({icon,title,detail,color=C.cyan})=><div style={{display:'flex',gap:20,alignItems:'center'}}><div style={{width:62,height:62,borderRadius:17,background:`${color}15`,display:'grid',placeItems:'center'}}><Icon name={icon} color={color}/></div><div><div style={{fontSize:30,fontWeight:600}}>{title}</div>{detail&&<div style={{fontSize:23,color:C.muted,marginTop:5}}>{detail}</div>}</div></div>;
