import {Interactive,useCurrentFrame} from 'remotion';
import {C,Orb,Stage,Tag,Heading,Card,Icon,enter} from '../design';
import {sceneSchema,type SceneProps} from './types';
const MemoryInner=({title,subtitle,style}:SceneProps)=>{
 const f=useCurrentFrame();const later=f>300;const controls=f>555;
 return <Stage duration={24} chapter="03 / PERSISTENT MEMORY" style={style}>
 <Heading title={title} subtitle={subtitle}/>
 <div style={{position:'absolute',left:100,top:355,display:'flex',gap:14}}><Tag>{later?'SESSION TWO':'SESSION ONE'}</Tag><Tag color={C.violet}>Mnemosyne built in</Tag><Tag color={C.green}>Saved locally</Tag></div>
 <Card style={{position:'absolute',left:100,top:445,width:730,height:350}} accent={C.cyan}>
  <div style={{fontSize:20,color:C.cyan,letterSpacing:2}}>{later?'PICK UP THE THREAD':'GIVE IT CONTEXT'}</div>
  <div style={{fontSize:37,lineHeight:1.45,marginTop:24}}>{later?'“Let’s plan the Aurora workshop.”':'“Aurora has a $600 budget. Keep the look blue and violet.”'}</div>
  <div style={{color:C.muted,fontSize:24,marginTop:25}}>{later?'The relevant context comes back.':'A conversation becomes a starting point.'}</div>
 </Card>
 <div style={{position:'absolute',left:830,top:450}}><Orb size={320}/></div>
 <div style={{position:'absolute',left:1170,top:410,width:630,display:'flex',flexDirection:'column',gap:18}}>
  {[
   {label:'PROJECT',value:'Aurora workshop',icon:'file',color:C.cyan},
   {label:'BUDGET',value:'$600',icon:'memory',color:C.green},
   {label:'STYLE',value:'Blue + violet',icon:'spark',color:C.violet},
  ].map((item,i)=><Card key={i} at={35+i*45} accent={item.color} style={{padding:'22px 27px',display:'flex',alignItems:'center',gap:22,transform:`translateX(${Math.sin(f/70+i)*4}px)`}}><Icon name={item.icon} color={item.color}/><div><div style={{fontSize:16,color:item.color,letterSpacing:2,marginBottom:7}}>{item.label}</div><div style={{fontSize:30,fontWeight:600}}>{item.value}</div></div><div style={{marginLeft:'auto'}}><Icon name="check" size={24} color={item.color}/></div></Card>)}
 </div>
 <div style={{position:'absolute',left:100,top:840,display:'flex',gap:37,opacity:enter(f,155)}}>{['Recent conversation','Meaning-based recall','Correct or forget'].map((x,i)=><div key={x} style={{fontSize:24,color:controls&&i===2?C.cyan:C.muted,display:'flex',gap:12,alignItems:'center'}}><Icon name="check" size={23} color={C.violet}/>{x}</div>)}</div>
 </Stage>
};
export const Memory=Interactive.withSchema({Component:MemoryInner,componentName:'<Memory>',schema:sceneSchema,wrapInSequence:true});
