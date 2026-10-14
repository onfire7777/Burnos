import {Interactive,useCurrentFrame} from 'remotion';
import {C,Stage,Tag,Heading,Card,Icon,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
import evidence from '../../public/memory-evidence.json';
type EvidenceTrace={trace:Array<{response:{result:{structuredContent?:{cid?:string;hits?:Array<{id:string;text:string}>}}}}>};
const recorded=evidence as unknown as EvidenceTrace;
const source=recorded.trace[recorded.trace.length-1].response.result.structuredContent?.hits?.[0];
const ProofInner=({title,subtitle,style}:SceneProps)=>{
 const f=useCurrentFrame();const step=f<115?0:f<205?1:2;
 return <Stage duration={14} chapter="04 / REAL BACKEND · RECORDED RESULT" label="REAL MCP OUTPUT · FICTIONAL TEST DATA" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <div style={{position:'absolute',left:100,top:365,display:'flex',gap:14}}><Tag color={C.green}>HTTP 200</Tag><Tag>MCP {evidence.protocol}</Tag><Tag color={C.violet}>SQLite persistence</Tag></div>
 <div style={{position:'absolute',left:100,top:455,width:510,display:'flex',flexDirection:'column',gap:25}}>{['01  Capture project context','02  Reopen the server','03  Retrieve the same source'].map((s,i)=><Card key={s} accent={step>=i?C.green:undefined} style={{padding:'27px 25px',fontSize:27,opacity:step>=i?1:.35,display:'flex',gap:20,alignItems:'center'}}><Icon name={step>=i?'check':'memory'} color={step>=i?C.green:C.muted}/>{s}</Card>)}</div>
 <Card accent={C.cyan} style={{position:'absolute',left:670,top:455,width:1130,height:358,fontFamily:'Consolas, monospace',padding:35}}>
  <div style={{fontSize:22,color:C.muted,borderBottom:`1px solid ${C.line}`,paddingBottom:24}}>{step===1?'APPLICATION LIFECYCLE':'tools/call'} <span style={{color:C.cyan,marginLeft:35}}>{step===0?'capture':step===1?'reopen → initialize':'search'}</span></div>
  {step===1?<div style={{fontSize:28,lineHeight:1.65,marginTop:25}}>New server instance.<br/>Same SQLite store.<br/><span style={{color:C.green}}>MCP 2025-11-25 initialized.</span></div>:<div style={{fontSize:25,lineHeight:1.6,marginTop:20}}><span style={{color:C.violet}}>{step===0?'content':'hits[0].text'}</span><div style={{marginTop:10}}>“{source?.text}”</div><div style={{fontSize:20,color:C.muted,marginTop:14}}>source ID: {source?.id.slice(0,24)}…</div></div>}
  <div style={{position:'absolute',bottom:25,left:35,color:C.green,fontSize:22,opacity:enter(f,225)}}>✓ same source ID returned after reopening</div>
 </Card>
 <div style={{position:'absolute',top:849,left:100,fontSize:23,color:C.muted}}>On-screen result abbreviated for readability. Full requests and responses included with the video.</div>
 </Stage>
};
export const Proof=Interactive.withSchema({Component:ProofInner,componentName:'<Proof>',schema:sceneSchema,wrapInSequence:true});
