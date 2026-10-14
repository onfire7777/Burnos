import {Composition,Folder} from 'remotion';
import {Film} from './Film';
import {Reveal} from './scenes/Reveal';
import {Conversation} from './scenes/Conversation';
import {Memory} from './scenes/Memory';
import {Proof} from './scenes/Proof';
import {Agents} from './scenes/Agents';
import {Research} from './scenes/Research';
import {Desktop} from './scenes/Desktop';
import {Connections} from './scenes/Connections';
import {Controls} from './scenes/Controls';
import {Architecture} from './scenes/Architecture';
import {Closing} from './scenes/Closing';

export const RemotionRoot=()=> <>
 <Composition id="Burnos-Official-Film" component={Film} width={1920} height={1080} fps={30} durationInFrames={5370}/>
 <Folder name="Scenes">
  <Composition id="01-Reveal" component={Reveal} width={1920} height={1080} fps={30} durationInFrames={360} defaultProps={{title:'Your next idea.\nAlready in context.',subtitle:'Meet the voice on your desktop, with a memory that stays.'}}/>
  <Composition id="02-Conversation" component={Conversation} width={1920} height={1080} fps={30} durationInFrames={390} defaultProps={{title:'Keep your flow. Just talk.',subtitle:'Talk naturally. Interrupt freely. Keep your flow.'}}/>
  <Composition id="03-Memory" component={Memory} width={1920} height={1080} fps={30} durationInFrames={720} defaultProps={{title:'Plot twist: it remembers.',subtitle:'Your context can outlast the conversation.'}}/>
  <Composition id="04-Proof" component={Proof} width={1920} height={1080} fps={30} durationInFrames={420} defaultProps={{title:'Close the session. Keep the context.',subtitle:'A real persistence test of the integrated memory backend.'}}/>
  <Composition id="05-Agents" component={Agents} width={1920} height={1080} fps={30} durationInFrames={570} defaultProps={{title:'Keep talking. Keep things moving.',subtitle:'Fast answers up front. Background agents on the task.'}}/>
  <Composition id="06-Research" component={Research} width={1920} height={1080} fps={30} durationInFrames={600} defaultProps={{title:'Meet Burnos’s research side.',subtitle:'A browser of its own. One less thing on your screen.'}}/>
  <Composition id="07-Desktop" component={Desktop} width={1920} height={1080} fps={30} durationInFrames={540} defaultProps={{title:'Say it. Make it happen.',subtitle:'From a useful answer to useful work.'}}/>
  <Composition id="08-Connections" component={Connections} width={1920} height={1080} fps={30} durationInFrames={480} defaultProps={{title:'Your tools. Now part of the conversation.',subtitle:'Extend Burnos with the services you connect.'}}/>
  <Composition id="09-Controls" component={Controls} width={1920} height={1080} fps={30} durationInFrames={540} defaultProps={{title:'A little orb. A lot of control.',subtitle:'Personalize how Burnos listens, speaks, and works.'}}/>
  <Composition id="10-Architecture" component={Architecture} width={1920} height={1080} fps={30} durationInFrames={480} defaultProps={{title:'Local foundations. Connected intelligence.',subtitle:'Built around the way you actually work.'}}/>
  <Composition id="11-Closing" component={Closing} width={1920} height={1080} fps={30} durationInFrames={270} defaultProps={{title:'burnos',subtitle:'Your next idea already has a head start.'}}/>
 </Folder>
</>;
