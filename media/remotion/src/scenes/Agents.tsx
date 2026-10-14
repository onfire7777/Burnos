import {Interactive,useCurrentFrame} from 'remotion';
import {C,Orb,Stage,Tag,Heading,Card,Icon,Wave,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
const AgentsInner=({title,subtitle,style}:SceneProps)=>{const f=useCurrentFrame();const steer=f>270;return <Stage duration={19} chapter="05 / TALK WHILE IT WORKS" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <Card style={{position:'absolute',left:100,top:355,width:730,height:445}} accent={C.cyan}>
  <Tag>THE CONVERSATION</Tag><div style={{display:'flex',alignItems:'center',gap:25,marginTop:15}}><Orb size={220}/><div><div style={{fontSize:33,fontWeight:600}}>Still here.</div><Wave bars={25}/></div></div>
  <div style={{fontSize:31,lineHeight:1.4}}>{steer?'“Keep it beginner friendly.”':'“Research workshop options while we shape the agenda.”'}</div>
 </Card>
 <div style={{position:'absolute',left:895,top:530,opacity:enter(f,65)}}><Icon name="arrow" size={66}/></div>
 <Card style={{position:'absolute',left:1030,top:355,width:770,height:445}} accent={C.violet} at={40}>
  <Tag color={C.violet}>THE BACKGROUND TASK</Tag>
  {['Gather workshop formats',steer?'Apply beginner-friendly constraint':'Compare agenda options','Build the Aurora shortlist'].map((x,i)=>{const p=enter(f,95+i*90,110);return <div key={i} style={{marginTop:30}}><div style={{display:'flex',alignItems:'center',gap:15,fontSize:28}}><Icon name={p>.99?'check':'spark'} color={p>.99?C.green:C.violet}/>{x}</div><div style={{height:5,background:'#243047',marginTop:16,marginLeft:47,borderRadius:9}}><div style={{height:5,width:`${p*100}%`,background:`linear-gradient(90deg,${C.violet},${C.cyan})`,borderRadius:9}}/></div></div>})}
 </Card>
 <div style={{position:'absolute',left:100,top:844,display:'flex',gap:20}}><Tag color={C.green}>Parallel progress</Tag><Tag color={C.violet}>Steer mid-task</Tag><Tag>Stop when you choose</Tag></div>
 </Stage>};
export const Agents=Interactive.withSchema({Component:AgentsInner,componentName:'<Agents>',schema:sceneSchema,wrapInSequence:true});
