import {Interactive,useCurrentFrame,interpolate} from 'remotion';
import {C,Stage,Heading,Card,Icon,Wave,clamp} from '../design';
import {sceneSchema,type SceneProps} from './types';
const DesktopInner=({title,subtitle,style}:SceneProps)=>{
 const f=useCurrentFrame();const dictating=f>290;const words='Join Aurora: a hands-on design workshop for beginners. Bring your laptop. We’ll bring the ideas.';const count=Math.round(interpolate(f,[300,440],[0,words.length],clamp));
 return <Stage duration={18} chapter="07 / VOICE TO ACTION" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <div style={{position:'absolute',left:100,top:370,width:510,display:'flex',flexDirection:'column',gap:24}}>
  {[['file','Work with files'],['code','Run commands'],['app','Use desktop controls'],['mic','Dictate into your app']].map(([icon,text],i)=><Card key={text} at={i*35} accent={dictating&&i===3?C.cyan:undefined} style={{display:'flex',alignItems:'center',gap:22,fontSize:28,padding:'25px 28px'}}><Icon name={icon}/>{text}</Card>)}
 </div>
 <Card style={{position:'absolute',left:665,top:370,width:1135,height:436,padding:0,overflow:'hidden'}} accent={dictating?C.green:C.violet}>
  <div style={{height:61,padding:'19px 30px',borderBottom:`1px solid ${C.line}`,fontSize:21,color:C.muted}}>AURORA LAUNCH / {dictating?'COPY DRAFT':'WORKSPACE'} <span style={{float:'right',color:C.green}}>{dictating?'DICTATION ACTIVE':'READY TO CREATE'}</span></div>
  <div style={{padding:38}}><div style={{fontSize:39,fontWeight:600}}>{dictating?'You’re invited to Aurora.':'Aurora / Workshop launch brief'}</div>
   {dictating?<div style={{fontSize:32,color:C.text,lineHeight:1.7,marginTop:23,minHeight:130}}>{words.slice(0,count)}<span style={{color:C.cyan,opacity:Math.sin(f*.22)>0?1:.2}}>|</span></div>:<div style={{display:'flex',gap:60,marginTop:28}}><div style={{fontSize:26,lineHeight:1.8,color:C.muted}}><span style={{color:C.cyan}}>90-MINUTE AGENDA</span><br/>15 min · welcome + demo<br/>50 min · create together<br/>25 min · share + reflect</div><div style={{fontSize:26,lineHeight:1.8,color:C.muted}}><span style={{color:C.green}}>$600 DEMO BUDGET</span><br/>Room $300 · materials $180<br/>Refreshments $120<br/><span style={{color:C.text}}>12 people · beginner friendly</span></div></div>}
   {dictating&&<Wave bars={45} height={43} color={C.green}/>}
  </div>
 </Card>
 <div style={{position:'absolute',left:670,top:844,fontSize:25,color:C.muted}}>{dictating?'Speak a command · or hold your transcription key':'From remembered constraints → to a usable plan'}</div>
 </Stage>
};
export const Desktop=Interactive.withSchema({Component:DesktopInner,componentName:'<Desktop>',schema:sceneSchema,wrapInSequence:true});
