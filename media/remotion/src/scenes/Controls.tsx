import {Interactive,useCurrentFrame} from 'remotion';
import {C,Orb,Stage,Tag,Heading,Card,Icon,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
const ControlsInner=({title,subtitle,style}:SceneProps)=>{
 const f=useCurrentFrame();const status=f<170?'Listening':f<255?'Wake-word standby':f<325?'Speaking':f<395?'Dictating':f<465?'Muted':'Stopped';const color=status==='Muted'?'#bdc9db':status==='Stopped'?'#ff5454':status==='Wake-word standby'?C.amber:status==='Dictating'?C.green:status==='Speaking'?'#1c68ff':C.cyan;
 return <Stage duration={18} chapter="09 / YOUR ASSISTANT. YOUR CONTROLS." style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <Card style={{position:'absolute',left:100,top:360,width:880,height:466}}>
  {[
   ['spark','Choose your models','Conversation + background tasks'],
   ['wave','Pick your voice','On-device voice generation'],
   ['mic','“Hey Burnos”','Optional wake word + speaker matching'],
  ].map(([icon,t,d],i)=><div key={t} style={{display:'flex',alignItems:'center',gap:23,padding:'23px 6px',borderBottom:i<2?`1px solid ${C.line}`:undefined,opacity:enter(f,i*50)}}><Icon name={icon} size={38} color={i%2?C.violet:C.cyan}/><div><div style={{fontSize:31,fontWeight:600}}>{t}</div><div style={{fontSize:23,color:C.muted,marginTop:7}}>{d}</div></div><div style={{marginLeft:'auto',width:52,height:28,borderRadius:20,background:C.cyan+'44'}}><div style={{width:20,height:20,background:C.cyan,borderRadius:20,margin:'4px 4px 4px auto'}}/></div></div>)}
 </Card>
 <div style={{position:'absolute',left:1195,top:330}}><Orb size={390} color={color}/></div>
 <div style={{position:'absolute',left:1140,top:706,width:520,textAlign:'center'}}><Tag color={color}>{status.toUpperCase()}</Tag><div style={{fontSize:26,color:C.muted,marginTop:25}}>{status==='Muted'?'One click · mute':status==='Stopped'?'Double click · stop all':'Color makes the state clear.'}</div></div>
 <div style={{position:'absolute',left:100,top:853,fontSize:24,color:C.muted}}>Voice commands · one-click mute · double-click stop</div>
 </Stage>
};
export const Controls=Interactive.withSchema({Component:ControlsInner,componentName:'<Controls>',schema:sceneSchema,wrapInSequence:true});
