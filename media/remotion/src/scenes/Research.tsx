import {Interactive,useCurrentFrame,interpolate} from 'remotion';
import {C,Orb,Stage,Tag,Heading,Card,Icon,enter,clamp} from '../design';
import {sceneSchema,type SceneProps} from './types';
const ResearchInner=({title,subtitle,style}:SceneProps)=>{
 const f=useCurrentFrame();const phase=Math.min(3,Math.floor(Math.max(0,f-40)/120));const labels=['Search the web','Open and explore','Read the PDF','Bring back the useful parts'];
 return <Stage duration={20} chapter="06 / THE NEW BROWSER" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <Tag style={{position:'absolute',left:100,top:333}} color={C.green}>NEW IN v0.22 · OWN BACKGROUND BROWSER</Tag>
 <div style={{position:'absolute',left:1010,top:345,display:'flex',gap:12}}>{['Search','Read','Find','PDFs'].map((t,i)=><Tag key={t} color={phase===i?C.cyan:C.muted}>{t}</Tag>)}</div>
 <div style={{position:'absolute',left:970,top:427,width:790,height:350,border:`1px solid ${C.line}`,borderRadius:24,transform:'rotate(4deg)',background:'#10192c'}}/>
 <Card style={{position:'absolute',left:960,top:420,width:840,height:375,padding:0,overflow:'hidden'}} accent={C.cyan}>
  <div style={{padding:'20px 27px',borderBottom:`1px solid ${C.line}`,display:'flex',alignItems:'center',gap:14,fontSize:20,color:C.muted}}><Icon name="web" size={23}/> Burnos research <span style={{marginLeft:'auto',color:C.green}}>TASK-OWNED TAB</span></div>
  <div style={{padding:'25px 35px'}}>
   <div style={{fontSize:30,fontWeight:600,marginBottom:28}}>{phase<2?'Aurora workshop · research':'Aurora-event-brief.pdf'}</div>
   {phase<2?<>{['Beginner-friendly workshop formats','Venue and equipment requirements','Hands-on agenda ideas'].map((t,i)=><div key={t} style={{display:'flex',gap:16,marginBottom:22,fontSize:24,opacity:enter(f,40+i*30)}}><Icon name="file" color={i===phase?C.cyan:C.muted} size={25}/>{t}</div>)}</>:<><div style={{display:'flex',gap:20,fontSize:25,marginBottom:24}}><Tag color={C.violet}>PDF TEXT</Tag><span style={{alignSelf:'center',color:C.muted}}>Key details found</span></div><div style={{fontSize:30,color:C.cyan,lineHeight:1.65}}>12 people · 90 minutes<br/><span style={{fontSize:25,color:C.text}}>Beginner friendly. Bring a laptop.</span></div></>}
  </div>
 </Card>
 <div style={{position:'absolute',left:100,top:440,width:740}}>
  <div style={{display:'flex',alignItems:'center',gap:25}}><Orb size={180}/><div style={{fontSize:41,lineHeight:1.2,fontWeight:600,maxWidth:420}}>{labels[phase]}</div></div>
  <div style={{fontSize:29,color:C.muted,lineHeight:1.55,marginTop:28}}>Your tabs stay yours.<br/>Burnos handles the research.</div>
  <div style={{width:500,height:5,background:C.line,marginTop:43}}><div style={{height:'100%',background:C.cyan,width:`${interpolate(f,[40,540],[0,100],clamp)}%`}}/></div>
 </div>
 <div style={{position:'absolute',left:100,top:842,fontSize:25,color:C.violet}}>Less tab juggling. More “oh, that’s useful.”</div>
 </Stage>;
};
export const Research=Interactive.withSchema({Component:ResearchInner,componentName:'<Research>',schema:sceneSchema,wrapInSequence:true});
